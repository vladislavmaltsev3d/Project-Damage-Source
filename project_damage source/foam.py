# создаём монтажную пену и отслеживаем изменения её источников
from . import quality_ui
from . import output_collections
from . import preview_quality
# ProjectDamage expanding foam - GPL-3.0-or-later; see LICENSE.
#
# Spray Foam: polyurethane foam on the object (core: foam.cpp). Result:
# "<source>_project_damage_foam" (parented to the source).
#   Place    Draw (default): foam along Grease Pencil (or curve) strokes,
#            projected onto the object's surface; New Drawing / Draw / Done /
#            Clear, Width. Gaps: foam squeezed out of the tight gaps of this
#            mesh itself (cracks, seams), never along open edges.
#   Gap      Max Gap / Min Gap: gap widths that get foam; Samples: surface
#            samples per m2 used to find them
#   Foam     Spacing, Blob Size, Bulge (swelling out of the mouth), Lumpiness,
#            Sag (fatter and lower on vertical seams), Min Openness (deeper in
#            the slit nothing is made: it would be hidden)
#   Shape    Style: Foam (foam-v2, one merged skin meshed from a distance
#            field) or Beads (foam-v1, a lumpy ellipsoid per blob); Spread
#            (overflow onto the faces), Cauliflower (bulbous lobes), Drips and
#            Drip Length (runs down walls, teardrops from ceilings), Coverage
#            (patchy bead), Trim (knife-cut patches), Resolution (mesh cells)
#   Look     Fresh / Cured / Aged / White / Grey / Fire-Rated presets, Color
#            and Age for the shared procedural material (foam_material.py)
#   Output   UV map "UVMap", attributes foam_variation, foam_cut,
#            foam_cavity and foam_coord (texture space), your Material or the
#            procedural "PD_Expanding Foam"
from .blender_compat import uses_nodes
from . import ui_image_hints
import bpy
import numpy as np
from mathutils import Vector

from . import core_loader


# здесь хранятся значения, которые сохраняются вместе с объектом
class ProjectDamageFoamSettings(bpy.types.PropertyGroup):
    placement: bpy.props.EnumProperty(name='Placement', default='GAPS', items=(
        ('DRAW','Draw','Foam along Grease Pencil strokes drawn on the object'),
        ('GAPS','Gaps','Foam squeezed out of the tight gaps (cracks, seams) of this mesh')))
    drawing: bpy.props.PointerProperty(name='Drawing', type=bpy.types.Object,
        poll=lambda self, ob: ob.type in {'GREASEPENCIL','GPENCIL','CURVE'},
        description='Grease Pencil (or curve) whose strokes are the foam path')
    width: bpy.props.FloatProperty(name='Width', default=0.03, min=0.002, soft_max=0.3, subtype='DISTANCE', description='Bead width of the drawn foam')
    max_gap: bpy.props.FloatProperty(name='Max Gap', default=0.02, min=0.0005, soft_max=0.2, subtype='DISTANCE', description='Gaps up to this wide get foam')
    min_gap: bpy.props.FloatProperty(name='Min Gap', default=0.0, min=0.0, soft_max=0.1, subtype='DISTANCE', description='Gaps narrower than this get none (0 = touching surfaces too)')
    samples: bpy.props.FloatProperty(name='Samples', default=40000.0, min=100.0, soft_max=400000.0, description='Surface samples per m2 used to find the gaps (more = finer, slower)')
    spacing: bpy.props.FloatProperty(name='Spacing', default=0.9, min=0.2, max=4.0, description='Blob spacing, x the gap width')
    min_openness: bpy.props.FloatProperty(name='Min Openness', default=0.08, min=0.0, max=0.6, description='Deeper in the slit than this nothing is made (it would be hidden)')
    blob_size: bpy.props.FloatProperty(name='Blob Size', default=1.6, min=0.2, max=5.0)
    bulge: bpy.props.FloatProperty(name='Bulge', default=1.2, min=0.0, max=4.0, description='How much the foam swells out of the gap mouth')
    lumpiness: bpy.props.FloatProperty(name='Lumpiness', default=1.0, min=0.0, max=3.0)
    sag: bpy.props.FloatProperty(name='Sag', default=0.6, min=0.0, max=3.0, description='On vertical seams the foam sags: lower and fatter below')
    max_blobs: bpy.props.IntProperty(name='Max Blobs', default=20000, min=10, max=500000)
    seed: bpy.props.IntProperty(name='Seed', default=0, min=0)
    material: bpy.props.PointerProperty(name='Material', type=bpy.types.Material)
    style: bpy.props.EnumProperty(name='Style', default='FOAM', items=(
        ('FOAM','Foam','One continuous skin with merged lobes, overflow, drips and cuts'),
        ('BEADS','Beads','Separate lumpy beads (the first Foam version)')))
    spread: bpy.props.FloatProperty(name='Spread', default=1.0, min=0.0, max=3.0, description='How far the foam overflows onto the faces round the gap')
    cauliflower: bpy.props.FloatProperty(name='Cauliflower', default=1.0, min=0.0, max=3.0, description='Bulbous lobes grown out of the bead, and smaller lobes out of those')
    drips: bpy.props.FloatProperty(name='Drips', default=0.35, min=0.0, max=1.0, description='Share of the bead that runs down a wall or hangs from a ceiling seam')
    drip_length: bpy.props.FloatProperty(name='Drip Length', default=2.5, min=0.0, max=10.0, description='Drip length, x the bead radius')
    coverage: bpy.props.FloatProperty(name='Coverage', default=1.0, min=0.0, max=1.0, subtype='FACTOR', description='1 = a continuous bead; lower leaves patches of the seam unfilled')
    trim: bpy.props.FloatProperty(name='Trim', default=0.0, min=0.0, max=1.0, subtype='FACTOR', description='Share of the bead cut flush with a knife, showing the open cells inside')
    resolution: bpy.props.FloatProperty(name='Resolution', default=0.14, min=0.04, max=0.6, description='Mesh cell size, x the bead radius (smaller = finer and heavier). Master Quality refines it')
    look: bpy.props.EnumProperty(name='Look', default='CURED', items=(
        ('FRESH','Fresh','Pale cream, just cured'),('CURED','Cured','Typical yellow polyurethane foam'),
        ('AGED','Aged','Yellowed to orange-brown by sunlight'),('WHITE','White','Off-white foam'),
        ('GREY','Grey','Grey foam'),('FIRE','Fire-Rated','Red-pink fire-rated foam'),('CUSTOM','Custom','Your Color and Age')))
    color: bpy.props.FloatVectorProperty(name='Color', subtype='COLOR', size=3, min=0.0, max=1.0, default=(0.80,0.56,0.20), description='Foam colour in the procedural material')
    age: bpy.props.FloatProperty(name='Age', default=0.12, min=0.0, max=1.0, subtype='FACTOR', description='UV yellowing toward orange-brown, stronger on exposed lobes')


_LOOK_PROPS=('look','color','age')
_LOOK_SYNC=[False]
_LOOK_INPUTS={}


def _look_input_signature(source,depsgraph):
    """Separate shader notifications from actual source geometry/input edits."""
    import hashlib
    h=hashlib.blake2b(digest_size=16)
    ev=source.evaluated_get(depsgraph);mesh=ev.data
    h.update(np.asarray(source.matrix_world,dtype=np.float64).tobytes())
    for seq,field,width,dtype in ((mesh.vertices,'co',3,np.float32),(mesh.loops,'vertex_index',1,np.int32),(mesh.polygons,'material_index',1,np.int32),(mesh.polygons,'loop_total',1,np.int32),(mesh.polygons,'use_smooth',1,bool)):
        data=np.empty(len(seq)*width,dtype);seq.foreach_get(field,data);h.update(data.tobytes())
    for attr in mesh.attributes:
        field,width={'FLOAT':('value',1),'INT':('value',1),'BOOLEAN':('value',1),'FLOAT_VECTOR':('vector',3),'FLOAT2':('vector',2),'FLOAT_COLOR':('color',4),'BYTE_COLOR':('color',4)}.get(attr.data_type,(None,0))
        h.update((attr.name+attr.domain+attr.data_type).encode())
        if field:
            data=np.empty(len(attr.data)*width,np.float64)
            try:attr.data.foreach_get(field,data);h.update(data.tobytes())
            except (TypeError,RuntimeError):pass
    h.update(repr([[(g.group,g.weight) for g in v.groups] for v in mesh.vertices]).encode())
    h.update(repr([slot.material.as_pointer() if slot.material else 0 for slot in source.material_slots]).encode())
    for name in sorted(p.identifier for p in source.bl_rna.properties if p.identifier.startswith('project_damage')):
        group=getattr(source,name,None)
        if not isinstance(group,bpy.types.PropertyGroup):continue
        for prop in group.bl_rna.properties:
            if prop.identifier=='rna_type' or (name=='project_damage_foam' and prop.identifier in _LOOK_PROPS):continue
            if prop.type in {'BOOLEAN','INT','FLOAT','STRING','ENUM'}:
                value=getattr(group,prop.identifier)
                if getattr(prop,'is_array',False):value=tuple(value)
                h.update((name+'.'+prop.identifier+repr(value)).encode())
    return h.digest()


def shader_only_update(source,depsgraph):
    saved=_LOOK_INPUTS.get(source.session_uid)
    if saved is None or source.type!='MESH':return False
    if saved==_look_input_signature(source,depsgraph):return True
    _LOOK_INPUTS.pop(source.session_uid,None)
    return False


def _look_changed(self, context):
    """Look only touches the output object's shader properties, never the geometry."""
    from . import foam_material,generated_assets
    from . import live
    if _LOOK_SYNC[0] or live._HISTORY_RESTORING:return
    _LOOK_SYNC[0]=True
    try:
        if self.look!='CUSTOM' and self.look in foam_material.LOOKS:
            color,age=foam_material.LOOKS[self.look]
            if tuple(round(c,4) for c in self.color)!=tuple(round(c,4) for c in color):self.color=color
            if abs(self.age-age)>1e-6:self.age=age
    finally:_LOOK_SYNC[0]=False
    previous=live._APPLYING;live._APPLYING=True
    try:
        foam_material.apply_look(generated_assets.output(self.id_data,'foam'),self.color,self.age)
                                                                               
                                                                           
        layer=getattr(context,'view_layer',None)
        if layer is not None:
            layer.update()
            _LOOK_INPUTS[self.id_data.session_uid]=_look_input_signature(self.id_data,context.evaluated_depsgraph_get())
    finally:live._APPLYING=previous


def _look_edited(self, context):
    """Editing Color or Age by hand switches Look to Custom."""
    from . import live
    if _LOOK_SYNC[0] or live._HISTORY_RESTORING:return
    _LOOK_SYNC[0]=True
    try:
        if self.look!='CUSTOM':self.look='CUSTOM'
    finally:_LOOK_SYNC[0]=False
    _look_changed(self,context)


def _settings_changed(self, context):
    from . import live
    live.touch_prop(self,context)


class ProjectDamageFoamNeighbor(bpy.types.PropertyGroup):
    obj: bpy.props.PointerProperty(name='Neighbor', type=bpy.types.Object,
        poll=lambda self, ob: ob.type=='MESH', update=_settings_changed)


                                                                               
ProjectDamageFoamSettings.__annotations__['neighbors']=bpy.props.CollectionProperty(type=ProjectDamageFoamNeighbor)
_INPUT_SIGNATURES={}
_DRAW_PENDING=[False]


def _migrate_gap_setups(*unused):
    from . import generated_assets,live
    try:objects=list(bpy.data.objects)
    except (AttributeError,RuntimeError):return None
    previous=live._APPLYING;live._APPLYING=True
    try:
        for source in objects:
            if source.type!='MESH' or not source.project_damage.foam_enable:continue
            settings=source.project_damage_foam
            if (not settings.is_property_set('placement') and settings.drawing is None
                    and (len(settings.neighbors) or generated_assets.output(source,'foam') is not None)):
                settings.placement='GAPS'
    finally:live._APPLYING=previous
    return None


from bpy.app.handlers import persistent
_migrate_gap_setups=persistent(_migrate_gap_setups)


def capture_selected(source, context, replace=False):
    """Neighboring mesh inputs were replaced by drawing; kept for callers."""
    return 0


def input_objects(source, target):
    from . import build,ownership
    if source.project_damage_foam.placement=='DRAW':return [target]
    objects=[target]
    for row in source.project_damage_foam.neighbors:
        obj=row.obj
        if obj is None or obj.type!='MESH' or obj==source:continue
        owner=ownership.source_of(obj)
        if owner==source:continue
        obj=build.result_of(owner) or obj
        if obj not in objects:objects.append(obj)
    return objects


DRAW_STAMP='project_damage_foam_drawing_of'


def drawing_strokes(drawing, frame=None):
    """World-space point lists, one per stroke, of a Grease Pencil or curve object."""
    strokes=[]
    if drawing is None:return strokes
    world=np.asarray(drawing.matrix_world,dtype=np.float64)
    def to_world(pts):
        pts=np.asarray(pts,np.float64).reshape(-1,3)
        return pts@world[:3,:3].T+world[:3,3]
    if drawing.type=='GREASEPENCIL':
        for layer in drawing.data.layers:
            world=np.asarray(drawing.matrix_world @ layer.matrix_local,dtype=np.float64)
            if layer.hide:continue
            fr=layer.current_frame() if frame is None else max((f for f in layer.frames if f.frame_number<=frame),key=lambda f:f.frame_number,default=None)
            if fr is None or fr.drawing is None:continue
            d=fr.drawing
            att=d.attributes.get('position')
            if att is None or not len(att.data):continue
            pos=np.empty(len(att.data)*3,np.float64);att.data.foreach_get('vector',pos)
            pos=to_world(pos)
            offs=[o.value for o in d.curve_offsets]
            for a,b in zip(offs[:-1],offs[1:]):
                if b-a>=2:strokes.append(pos[a:b])
    elif drawing.type=='GPENCIL':
        for layer in drawing.data.layers:
            fr=layer.active_frame
            if fr is None:continue
            for st in fr.strokes:
                if len(st.points)>=2:strokes.append(to_world([p.co[:] for p in st.points]))
    elif drawing.type=='CURVE':
        from mathutils import geometry
        for sp in drawing.data.splines:
            if sp.type=='BEZIER':
                bp=list(sp.bezier_points)
                if sp.use_cyclic_u and bp:bp.append(bp[0])
                pts=[]
                for a,b in zip(bp[:-1],bp[1:]):
                    seg=geometry.interpolate_bezier(a.co,a.handle_right,b.handle_left,b.co,max(2,sp.resolution_u+1))
                    pts+= [v[:] for v in (seg if not pts else seg[1:])]
            else:
                pts=[p.co[:3] for p in sp.points]
                if sp.use_cyclic_u and pts:pts.append(pts[0])
            if len(pts)>=2:strokes.append(to_world(pts))
    return strokes


def _resample(stroke, step):
    seg=np.linalg.norm(np.diff(stroke,axis=0),axis=1)
    keep=np.concatenate([[True],seg>1e-9]);stroke=stroke[keep];seg=seg[seg>1e-9]
    if len(stroke)<2:return stroke
    s=np.concatenate([[0.],np.cumsum(seg)])
    n=max(2,int(np.ceil(s[-1]/max(step,1e-6)))+1)
    t=np.linspace(0.,s[-1],n)
    return np.stack([np.interp(t,s,stroke[:,k]) for k in range(3)],axis=1)


def _signature(source, depsgraph):
    import hashlib
    from . import build
    target=build.result_of(source) or source
    h=hashlib.blake2b(digest_size=16)
    drawing=source.project_damage_foam.drawing
    if source.project_damage_foam.placement=='DRAW' and drawing is not None:
        from . import live_input_state
        h.update(repr(live_input_state.signature(drawing)).encode())
    for obj in input_objects(source,target):
        ev=obj.evaluated_get(depsgraph)
        mesh=ev.data
        if obj not in {source,target}:h.update(np.asarray(obj.matrix_world,dtype=np.float64).tobytes())
        for seq,field,width,dtype in ((mesh.vertices,'co',3,np.float32),(mesh.loops,'vertex_index',1,np.int32),(mesh.polygons,'loop_total',1,np.int32)):
            values=np.empty(len(seq)*width,dtype=dtype);seq.foreach_get(field,values)
            h.update(str(len(seq)).encode());h.update(values.tobytes())
    return h.digest()


def clear_pending():
    _INPUT_SIGNATURES.clear()
    _LOOK_INPUTS.clear()
    _DRAW_PENDING[0]=False


def dependencies_changed(scene, depsgraph):
    from . import live,build
                                                                         
    if bpy.context.mode not in {'OBJECT','PAINT_GREASE_PENCIL','PAINT_GPENCIL'}:
        _DRAW_PENDING[0]=True
        return
    pending=_DRAW_PENDING[0];_DRAW_PENDING[0]=False
    updated={u.id.original for u in depsgraph.updates if isinstance(u.id,bpy.types.Object)
             and (u.is_updated_transform or u.is_updated_geometry)}
    moving={u.id.original for u in depsgraph.updates if u.is_updated_transform and isinstance(u.id.original,bpy.types.Object) and u.id.original.mode=='OBJECT'}
    mesh_updates={u.id.original for u in depsgraph.updates if not isinstance(u.id,bpy.types.Object)
                  and u.is_updated_geometry}
    if not updated and not mesh_updates and not pending:return
    for source in scene.objects:
        if source.type!='MESH' or not source.project_damage.foam_enable or source.project_damage.all_off:
            continue
        if not source.get('project_damage_uid') or source.get('project_damage_bfrag_stage'):continue
        target=build.result_of(source) or source
        watched={source,*input_objects(source,target)}
        watched.difference_update({source,target} & moving)
        settings=source.project_damage_foam
        if settings.placement=='GAPS':watched.update(row.obj for row in settings.neighbors if row.obj is not None)
        if live.drawing_exit_suppressed(source):
            _INPUT_SIGNATURES[source.session_uid]=_signature(source,depsgraph)
            continue
        if settings.placement=='DRAW' and settings.drawing is not None:watched.add(settings.drawing)
        if not pending and not updated.intersection(watched) and not any(ob.data in mesh_updates for ob in watched):continue
        signature=_signature(source,depsgraph)
        if _INPUT_SIGNATURES.get(source.session_uid)!=signature:
            _INPUT_SIGNATURES[source.session_uid]=signature
            live._mark_dirty(source)


def _set_params(p, values):
    """Fields the loaded core does not know yet are skipped (older binary)."""
    missing=[]
    for name,value in values.items():
        if hasattr(p,name):setattr(p,name,value)
        else:missing.append(name)
    return missing


def _foam_along_strokes(core, p, points, polys, strokes, spacing):
    """Strokes resampled, snapped onto the surface (nearest point, its normal), then foam."""
    from mathutils.bvhtree import BVHTree
    tree=BVHTree.FromPolygons([tuple(v) for v in points],[tuple(f[:3]) for f in polys],all_triangles=True)
    guidance=surface_guidance.field(points,polys)
    step=max(.2,spacing)*.6*p.max_gap
    cs,ns,ts=[],[],[]
    for st in strokes:
        st=_resample(st,step)
        if len(st)<2:continue
        snapped=[];normals=[]
        for q in st:
            hit=tree.find_nearest(Vector(q))
            if hit[0] is None:continue
            snapped.append(hit[0][:]);normals.append(surface_guidance.face_normal(guidance,hit[2],hit[1])[:])
        if len(snapped)<2:continue
        c=np.asarray(snapped);n=np.asarray(normals)
        t=np.gradient(c,axis=0)
        cs.append(c);ns.append(n);ts.append(t)
    if not cs:return None
    arr=lambda xs:np.ascontiguousarray(np.vstack(xs),dtype=np.float64)
    return arr(cs),arr(ns),arr(ts)


def generate(context,source,target,s,reports):
    from . import background_compute as bg,generated_assets
    if bg.async_for(source):
        def work():
            messages=[]
            result=yield from _generate_steps(bpy.context,source,target,s,messages)
            for flags,message in messages:
                if 'ERROR' in flags:raise RuntimeError(message)
            if result is None:
                old=generated_assets.output(source,'foam')
                if old is not None:old.hide_set(True);old.hide_render=True
            return result
        bg.queue(source,'generate_foam',work,guard=lambda:bg.valid_id(target))
        return generated_assets.output(source,'foam')
    return bg.immediate(_generate_steps(context,source,target,s,reports))

def _generate_steps(context, source, target, s, reports):
    from . import background_compute as bg
    from . import generated_assets,foam_material
    if not s.is_property_set('placement') and s.drawing is None and len(s.neighbors):
        from . import live
        previous=live._APPLYING;live._APPLYING=True
        try:s.placement='GAPS'
        finally:live._APPLYING=previous
    core=core_loader.load()
    if core is None or not hasattr(core,'spray_foam'):
        reports.append(({'ERROR'},'Expanding Foam: restart Blender to load the updated native core'))
        return None
    draw=s.placement=='DRAW'
    if not s.is_property_set('placement'):
        from . import live
        previous=live._APPLYING;live._APPLYING=True
        try:s.placement='DRAW' if draw else 'GAPS'
        finally:live._APPLYING=previous
    if draw and context.mode in {'PAINT_GREASE_PENCIL','PAINT_GPENCIL'} and context.active_object==s.drawing:
        return generated_assets.output(source,'foam')
    if draw and not hasattr(core,'foam_path'):
        reports.append(({'ERROR'},'Expanding Foam: rebuild the native core and restart Blender to draw foam'))
        return None
    strokes=drawing_strokes(s.drawing) if draw else []
    if draw and not strokes:
        reports.append(({'INFO'},'Expanding Foam: draw the foam path (New Drawing, then Draw)'))
        return None
    deps=context.evaluated_depsgraph_get()
    all_pts=[];all_polys=[];offset=0
    for obj in input_objects(source,target):
        ev=obj.evaluated_get(deps);mesh=ev.to_mesh()
        try:
            co=np.empty(len(mesh.vertices)*3,np.float64);mesh.vertices.foreach_get('co',co)
            world=np.asarray(obj.matrix_world,dtype=np.float64)
            all_pts.append(co.reshape(-1,3) @ world[:3,:3].T + world[:3,3])
                                                                                        
            mesh.calc_loop_triangles()
            mirrored=np.linalg.det(world[:3,:3])<0
            tri=np.empty(len(mesh.loop_triangles)*3,np.int32)
            mesh.loop_triangles.foreach_get('vertices',tri)
            tri=tri.reshape(-1,3)+offset
            if mirrored:tri=tri[:,[0,2,1]]
            all_polys.append(tri[:,[0,1,2,2]])
            offset+=len(mesh.vertices)
        finally:ev.to_mesh_clear()
    if not all_pts or not all_polys:return None
    points=np.ascontiguousarray(np.vstack(all_pts),dtype=np.float64)
    polys=np.ascontiguousarray(np.vstack(all_polys),dtype=np.int32).reshape(-1,4)
    scale=max(source.project_damage.global_scale/100.0,.0001)
    quality=max(preview_quality.percent(source.project_damage)/100.0,.0001)
    p=core.FoamParams()
    p.seed=(source.project_damage.seed+s.seed)%2147483647
    p.max_gap=max(s.min_gap,s.max_gap)*scale
    p.min_gap=min(s.min_gap,s.max_gap)*scale
    p.density=s.samples*quality/(scale*scale)
    for name in ('spacing','min_openness','blob_size','bulge','lumpiness','sag','max_blobs'):
        setattr(p,name,getattr(s,name))
    p.up_x,p.up_y,p.up_z=0.,0.,1.
    if draw:p.max_gap=p.min_gap=0.;p.max_gap=s.width*scale/1.4
    missing=_set_params(p,{'style':0 if s.style=='BEADS' else 1,'spread':s.spread,'cauliflower':s.cauliflower,
        'drips':s.drips,'drip_length':s.drip_length,'coverage':s.coverage,'trim':s.trim,
        'resolution':s.resolution/max(quality,.25)**.5})
    if missing and s.style!='BEADS':
        reports.append(({'WARNING'},'Expanding Foam: rebuild the native core and restart Blender for the Foam style; Beads are shown'))
    if draw:
        path=_foam_along_strokes(core,p,points,polys,strokes,s.spacing)
        r=(yield from bg.native(core,'foam_path',*path,p)) if path is not None else None
        if r is None:
            reports.append(({'INFO'},'Expanding Foam: the drawing is empty'))
            return None
    else:r=yield from bg.native(core,'spray_foam',points,polys,p,**surface_guidance.kwargs(core))
    _INPUT_SIGNATURES[source.session_uid]=_signature(source,deps)
    if not len(r['polys']):
        reports.append(({'INFO'},'Expanding Foam: nothing on the drawing' if draw else 'Expanding Foam: no tight gap found in this mesh; increase Max Gap or use Draw'))
        return None
    inverse=np.asarray(target.matrix_world.inverted_safe(),dtype=np.float64)
    fp=r['points'] @ inverse[:3,:3].T + inverse[:3,3]
                                       
    polys=np.asarray(r['polys'],np.int32).reshape(-1,4)
    tri=polys[:,3]==polys[:,2]
    keep=np.ones(polys.shape,bool)
    if np.linalg.det(inverse[:3,:3])<0:
                                                                                
        polys=polys[:,::-1];keep[:,0]=~tri
    else:keep[:,3]=~tri
    loops=polys[keep]
    totals=keep.sum(1).astype(np.int32)
    mesh=bpy.data.meshes.new(source.name+'_project_damage_foam')
    mesh.vertices.add(len(fp));mesh.vertices.foreach_set('co',fp.astype(np.float32).ravel())
    count=len(totals);mesh.loops.add(len(loops));mesh.polygons.add(count)
    mesh.loops.foreach_set('vertex_index',loops.astype(np.int32))
    starts=np.zeros(count,np.int32);starts[1:]=np.cumsum(totals)[:-1]
    mesh.polygons.foreach_set('loop_start',starts)
    try:mesh.polygons.foreach_set('loop_total',totals)
    except (AttributeError,RuntimeError):pass
    mesh.update(calc_edges=True)
    uv=mesh.uv_layers.new(name='UVMap')
    uv.data.foreach_set('uv',np.asarray(r['uvs'])[loops].astype(np.float32).ravel())
    attr=mesh.attributes.new('foam_variation','FLOAT','POINT')
    attr.data.foreach_set('value',np.asarray(r['variation'],np.float32))
    for key,name in (('cut','foam_cut'),('cavity','foam_cavity')):
        values=np.asarray(r[key],np.float32) if key in r and len(r[key])==len(fp) else np.zeros(len(fp),np.float32)
        mesh.attributes.new(name,'FLOAT','POINT').data.foreach_set('value',values)
                                                                                     
    world=np.asarray(target.matrix_world,dtype=np.float64)
    u,_,vt=np.linalg.svd(world[:3,:3]);rot=u@vt
    coord=(np.asarray(r['points'],np.float64)-world[:3,3])@rot
    mesh.attributes.new('foam_coord','FLOAT_VECTOR','POINT').data.foreach_set('vector',coord.astype(np.float32).ravel())
    mesh.shade_smooth()
    mat=s.material
    if mat is None:mat=foam_material.ensure_material()
    mesh.materials.append(mat)
    obj=generated_assets.place(context,source,target,'foam',mesh)
    foam_material.apply_look(obj,s.color,s.age)
    reports.append(({'INFO'},'Expanding Foam: %d blobs'%r['blobs']))
    return obj


def _new_drawing(context, source):
    data=bpy.data.grease_pencils.new(source.name+'_FoamDrawing')
    layer=data.layers.new('Foam',set_active=True)
    layer.frames.new(context.scene.frame_current)
    obj=bpy.data.objects.new(source.name+'_FoamDrawing',data)
    output_collections.link_object(context,source,obj)
    obj[DRAW_STAMP]=source.get('project_damage_uid','')
    obj['project_damage_paint_source']=source
    obj.parent=source
    mat=bpy.data.materials.new(obj.name+'_Ink');bpy.data.materials.create_gpencil_data(mat)
    mat.grease_pencil.color=(.05,.5,1.,1.);data.materials.append(mat)
    obj.hide_render=True
    return obj


# действие «Foam Drawing»
class PROJECT_DAMAGE_OT_foam_draw(bpy.types.Operator):
    bl_idname='project_damage.foam_draw'
    bl_label='Foam Drawing'
    bl_description='Draw the foam path with Grease Pencil on the object'
    bl_options={'REGISTER','UNDO'}
    action: bpy.props.EnumProperty(items=(
        ('NEW','Add New','Make a Grease Pencil drawing for the foam and start drawing'),
        ('DRAW','Draw','Draw foam strokes onto the object'),
        ('DONE','Finish Drawing','Leave Draw mode without automatically rebuilding geometry'),
        ('CLEAR','Delete','Delete the linked drawing object'),
        ('UPDATE','Update','Finish drawing and rebuild foam from the current spline now')))
    source_name: bpy.props.StringProperty(options={'HIDDEN'})

    def execute(self,context):
        from . import ownership,live
        source=bpy.data.objects.get(self.source_name) if self.source_name else ownership.context_source(context)
        if source is None or source.type!='MESH':
            source=next((o for o in bpy.data.objects if o.type=='MESH' and o.get('project_damage_uid')
                         and context.active_object is not None and o.get('project_damage_uid')==context.active_object.get(DRAW_STAMP)),None)
        if source is None:return {'CANCELLED'}
        s=source.project_damage_foam
        if self.action=='UPDATE':
            from . import paint,build,background_compute
            previous=live._APPLYING;live._APPLYING=True
            try:
                if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
                live.finish_drawing(source)
                _DRAW_PENDING[0]=False
                paint._activate(context,build.result_of(source) or source)
                context.view_layer.update()
                source.project_damage.foam_enable=True
                source.project_damage.all_off=False
                with background_compute.synchronous():
                    result,reports=build.rebuild_object(context,source)
                for level,message in reports:self.report(level,message)
                return {'FINISHED'} if result is not None else {'CANCELLED'}
            except Exception as exc:
                self.report({'ERROR'},'Foam update failed: '+str(exc))
                return {'CANCELLED'}
            finally:
                live._APPLYING=previous
        if context.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
        if self.action=='CLEAR':
            drawing=s.drawing
            if drawing is not None:
                if drawing.library is not None:
                    self.report({'WARNING'},'Linked drawings cannot be deleted here');return {'CANCELLED'}
                from . import paint,build
                paint._activate(context,build.result_of(source) or source)
                s.drawing=None
                bpy.data.objects.remove(drawing,do_unlink=True)
            return {'FINISHED'}
        if self.action=='DONE':
            from . import paint,build
            paint._activate(context,build.result_of(source) or source)
            _DRAW_PENDING[0]=False
            live.finish_drawing(source)
            return {'FINISHED'}
        if self.action=='NEW' or s.drawing is None:
            previous=live._APPLYING;live._APPLYING=True
            try:
                s.drawing=_new_drawing(context,source);s.placement='DRAW'
            finally:live._APPLYING=previous
        drawing=s.drawing
        from . import paint
        drawing.hide_set(False);drawing.hide_viewport=False
        if drawing.type in {'GREASEPENCIL','GPENCIL'}:
            paint._start_drawing(context,drawing)
        else:
            paint._activate(context,drawing)
            if not bpy.app.background:bpy.ops.object.mode_set(mode='EDIT')
        self.report({'INFO'},'Draw on the object, then press Update to rebuild the foam')
        return {'FINISHED'}


class PROJECT_DAMAGE_OT_spray_foam(bpy.types.Operator):
    bl_idname='project_damage.spray_foam'
    bl_label='Spray Foam'
    bl_description='Generate foam along the drawing, or inside the narrow gaps of this mesh'
    bl_options={'REGISTER','UNDO'}

    @classmethod
    def poll(cls,context):
        from . import ownership
        source=ownership.context_source(context)
        return context.mode=='OBJECT' and source is not None and source.type=='MESH'

    def execute(self,context):
        from . import ownership,build
        return build._apply_extra(context,self,'foam')


class _PANEL:
    def draw(self,context):
        from . import material_controls
        layout=self.layout;s=context.active_object.project_damage_foam
        source=context.active_object
        if ui_image_hints.enabled():layout.operator('project_damage.foam_help',text='Why no foam?',icon='QUESTION')
        box=layout.box();material_controls.centered_heading(box,'Placement',icon='GREASEPENCIL')
        ui_image_hints.prop(box.row(align=True),s,'placement',expand=True)
        if s.placement=='DRAW':
            box.prop(s,'drawing',text='Drawing')
            row=box.row(align=True)
            drawing_action=('DRAW','Draw','GREASEPENCIL') if s.drawing is not None else ('NEW','Add New','ADD')
            for action,text,icon in (drawing_action,('UPDATE','Update','FILE_REFRESH')):
                op=row.operator('project_damage.foam_draw',text=text,icon=icon);op.action=action;op.source_name=source.name
            if s.drawing is not None:
                op=row.operator('project_damage.foam_draw',text='',icon='TRASH');op.action='CLEAR';op.source_name=source.name
            box.prop(s,'width',text='Width')
        else:
            col=box.column(align=True)
            col.prop(s,'max_gap',text='Max Gap');col.prop(s,'min_gap',text='Min Gap')
            col.prop(s,'samples',text='Samples');col.prop(s,'min_openness',text='Min Openness')
        box=layout.box();material_controls.centered_heading(box,'Foam',icon='META_BALL')
        col=box.column(align=True)
        material_controls.inline(box, s, 'material', text='Foam')
        col.prop(s,'spacing',text='Spacing');col.prop(s,'blob_size',text='Blob Size')
        col.prop(s,'bulge',text='Bulge');col.prop(s,'lumpiness',text='Lumpiness')
        col.prop(s,'sag',text='Sag')
        if s.style!='BEADS':
            col=box.column(align=True)
            col.prop(s,'spread',text='Spread');col.prop(s,'cauliflower',text='Cauliflower')
            col.prop(s,'drips',text='Drips')
            if s.drips>0:col.prop(s,'drip_length',text='Drip Length')
            col.prop(s,'coverage',text='Coverage');col.prop(s,'trim',text='Trim')
            quality_ui.draw(col, s,'resolution',text='Resolution')
        col=box.column(align=True)
        if material_controls.experimental():
            draw_material_settings(layout, bpy.context)
        col.prop(s,'max_blobs',text='Max Blobs');col.prop(s,'seed',text='Seed')


def uses_builtin_material(material):
    if material is None:return True
    from .material_uv import base_material
    from .foam_material import MATERIAL
    material=base_material(material)
    library=material.get('project_damage_library_file','')
    return (material.get('project_damage_foam_library_material')==MATERIAL or material.name==MATERIAL
            or (__import__('pathlib').Path(library).name=='foam_mat.blend'
                and __import__('re').sub(r'\.\d{3}$','',material.name)==MATERIAL))


def draw_material_settings(layout,context):
    from . import ownership,material_controls
    source=ownership.context_source(context)
    if source is None:return
    s=source.project_damage_foam
    if not uses_builtin_material(s.material):return
    box=layout.box();material_controls.centered_heading(box,'Look',icon='SHADING_RENDERED')
    selector=box.grid_flow(row_major=True,columns=3,even_columns=True,even_rows=True,align=True)
    selector.scale_y=1.5
    for item in s.bl_rna.properties['look'].enum_items:
        selector.prop_enum(s,'look',item.identifier,text=item.name)
    col=box.column(align=True)
    col.prop(s,'color',text='Color');col.prop(s,'age',text='Age')
    if s.material is not None:box.label(text='Your Material is used; Look sets the built-in one',icon='INFO')
    from . import generated_assets
    output=generated_assets.output(source,'foam')
    material=s.material or (output.active_material if output is not None else None)
    if material is not None and uses_nodes(material):
        node=next((n for n in material.node_tree.nodes if n.type=='GROUP'
                   and n.node_tree and n.node_tree.get('pd_foam_shader')),None)
        if node is not None:
            box=layout.box();material_controls.centered_heading(box,'Shader Settings',icon='MATERIAL')
            col=box.column(align=True)
            for socket in node.inputs:
                if socket.name not in {'Color','Age'} and hasattr(socket,'default_value') and not socket.is_linked:
                    col.prop(socket,'default_value',text=socket.name)

_CLASSES=(ProjectDamageFoamNeighbor,ProjectDamageFoamSettings,PROJECT_DAMAGE_OT_foam_draw,PROJECT_DAMAGE_OT_spray_foam)
# подключаем свойства и действия этого модуля к Blender
def register():
    from . import live,generated_assets
    for name,prop in ProjectDamageFoamSettings.__annotations__.items():
        if name=='neighbors':continue
        if name=='look':prop.keywords['update']=_look_changed
        elif name in _LOOK_PROPS:prop.keywords['update']=_look_edited
        else:prop.keywords['update']=generated_assets.material_changed if name=='material' else live.touch_prop
    for cls in _CLASSES:bpy.utils.register_class(cls)
    bpy.types.Object.project_damage_foam=bpy.props.PointerProperty(type=ProjectDamageFoamSettings)
    if _migrate_gap_setups not in bpy.app.handlers.load_post:bpy.app.handlers.load_post.append(_migrate_gap_setups)
    _migrate_gap_setups()
    if not bpy.app.timers.is_registered(_migrate_gap_setups):bpy.app.timers.register(_migrate_gap_setups,first_interval=0.1)


# убираем подключения модуля при выключении аддона
def unregister():
    clear_pending()
    if _migrate_gap_setups in bpy.app.handlers.load_post:bpy.app.handlers.load_post.remove(_migrate_gap_setups)
    if bpy.app.timers.is_registered(_migrate_gap_setups):bpy.app.timers.unregister(_migrate_gap_setups)
    if hasattr(bpy.types.Object,'project_damage_foam'):del bpy.types.Object.project_damage_foam
    for cls in reversed(_CLASSES):
        if cls.is_registered:bpy.utils.unregister_class(cls)

                                                
from . import surface_guidance
