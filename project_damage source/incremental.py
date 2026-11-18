# повторно используем промежуточную геометрию неизменённых эффектов
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact session intermediates, partitioned by generator dependencies.

No mesh approximation and no resolution changes. An upstream geometry edit
invalidates dependent stages; an effect edit leaves unrelated cutters reusable.
"""
import hashlib, os, struct
import bpy
import numpy as np
from . import background_wire, preview_quality


def preference(name, default=True):
    entry=bpy.context.preferences.addons.get(__package__)
    return bool(getattr(entry.preferences,name,default)) if entry else default


def cache_enabled():return preference('cache_intermediates')
def enabled():return cache_enabled() and preference('incremental_updates')


def digest(value):
    h=hashlib.blake2b(digest_size=8)
    def put(x):
        if x is None:h.update(b'N')
        elif isinstance(x,(bool,np.bool_)):h.update(b'T' if x else b'F')
        elif isinstance(x,(int,np.integer)):
            b=str(int(x)).encode();h.update(b'I'+struct.pack('<Q',len(b))+b)
        elif isinstance(x,(float,np.floating)):h.update(b'D'+struct.pack('<d',float(x)))
        elif isinstance(x,str):
            b=x.encode();h.update(b'S'+struct.pack('<Q',len(b))+b)
        elif isinstance(x,(bytes,bytearray)):h.update(b'B'+struct.pack('<Q',len(x)));h.update(x)
        elif isinstance(x,np.ndarray):
            a=np.ascontiguousarray(x);put((a.dtype.str,a.shape))
            if a.size:h.update(memoryview(a).cast('B'))
        elif isinstance(x,dict):
            h.update(b'{')
            for key in sorted(x):put(key);put(x[key])
            h.update(b'}')
        elif isinstance(x,(list,tuple)):
            h.update(b'['+struct.pack('<Q',len(x)))
            for v in x:put(v)
        else:raise TypeError('Unsupported incremental fingerprint: '+str(type(x)))
    put(value)
    return int.from_bytes(h.digest(),'little') or 1


def _resources(value):
    if isinstance(value,str) and value.startswith(('/','~')):
        from pathlib import Path
        try:
            path=Path(bpy.path.abspath(value)).expanduser();stat=path.stat()
            if path.is_file():return (value,stat.st_mtime_ns,stat.st_size)
        except OSError:pass
    if isinstance(value,dict):return {k:_resources(v) for k,v in value.items()}
    if isinstance(value,(list,tuple)):return [_resources(v) for v in value]
    return value


                                                                             
                                                                                
_NESTED={'fabric':9,'concrete':10,'hit':11,'wood':12,'lath':13,'wallpaper':14,
         'bullet':15,'blast':16,'ground':17,'brick':18,'glass':19,'fissure':20,
         'bevel':21,'damaged_bevel':21,'scratch':22,'wall_hole':26,'hang':26,
         'rusty':27,'erosion':32,'metal_tear':34}
_EDGE={'amount','size_min','size_max','depth_min','depth_max','detail','stretch',
       'offset','random_offset','noise_strength','noise_scale','noise_roughness','noise_distortion','norm_enable','norm_max_size',
       'sec_amount','sec_size','sec_depth','avoid_chips'}
_IGNORE={'incremental_keys','profile_core','damage_shading','damage_smooth_angle',
         'base_smooth_angle','fract_tile_offset','fract_tile_seed','breakers',
         'break_rebar','break_rim_amount','break_rim_size','break_rim_depth','break_surf_dmg',
         'debris','cellf','rbf'}
_STAGES=(1,2,3,4,5,28,29,6,7,8,26,9,10,11,12,13,14,15,16,17,18,19,20,21,22,27,32,34)
_INPUTS={'guide_splines':4,'guide_levels':4,'surface_paint_paths':3,'surface_paint_normals':3,
         'concrete_strokes':10,'fabric_weights':9,'fabric_avoid_polys':9,
         'coating_paint_paths':8,'coating_paint_normals':8,'rust_paint_paths':7,'rust_paint_normals':7,
         'heat_weights':6,'rust_weights':7,
         'chip_strokes':1,'chip_paint_radius':1,'wood_strokes':12,'lath_strokes':13,
         'wallpaper_paths':14,'wallpaper_normals':14,'fabric_holes':9,'fabric_hole_normals':9,
         'fabric_edges':9,'fabric_edge_normals':9,'bullet_strokes':15,'ground_strokes':17,
         'brick_strokes':18,'fissure_strokes':20,'rusty_strokes':27,'glass_holes':19,
         'glass_hole_normals':19,'glass_impacts':19,'glass_impact_normals':19,
         'bevel_strokes':21,'damaged_bevel_strokes':21,'scratch_strokes':22,
         'fract_helper_strokes':4,'rbf_cut_strokes':23,'rbf_impact_strokes':23,'cellf_strokes':24}


def _groups(name):
    if name in _NESTED:return (_NESTED[name],)
    if name in _EDGE or name.startswith('chip_'):return (1,)
    if name.startswith('corner_') or name=='avoid_corner':return (2,)
    if name.startswith('surf_') or name=='avoid_surf':return (3,9,14,26) if name=='surf_paint_snap' else (3,)
    if name.startswith('fract') or name=='avoid_fract':return (4,)
    if name.startswith('build_corner') or name.startswith('pile_'):return (28,)
    if name.startswith('build_'):return (5,28) if name in {'build_paint_only','build_paint_pts','build_paint_count','build_paint_radius'} else (5,)
    if name=='avoid_build':return (5,28)
    if name.startswith('deposit_'):return (29,)
    if name.startswith('heat_'):return (6,)
    if name.startswith('rust_'):return (7,)
    if name.startswith('coating_'):return (8,)
    return ()


def prepare(source,core,params,points,polys,**inputs):
    """Fill native per-generator keys after ALL settings/drawings are resolved."""
    if not hasattr(params,'incremental_keys'):return
    if not enabled():params.incremental_keys=[];return
    encoded=_resources(background_wire.encode(params,core)['fields'])
    shared={};stages={key:{} for key in _STAGES}
    for name,value in encoded.items():
        if name in _IGNORE:continue
        owners=_groups(name)
        if name == 'brick':
            fields = value['fields']
            value = {**value, 'fields': {
                field: item for field, item in fields.items()
                if field != 'surf_mode' and not field.startswith(('tex_', 'noise_'))
            }}
        if owners:
            for key in owners:stages[key][name]=value
        else:shared[name]=value
    common_inputs={}
    for name,value in inputs.items():
        if name in _INPUTS:
            if _INPUTS[name] in stages:stages[_INPUTS[name]][name]=background_wire.encode(value,core)
        else:common_inputs[name]=background_wire.encode(value,core)
                                                                           
    common=digest((source.session_uid,shared,points,polys,common_inputs))
    keys=[0]*41;keys[0]=common
    for key,values in stages.items():keys[key]=digest((common,key,values))
    base_fields={k:v for k,v in shared.items() if k not in {'plaster','pits','metal','clay'}}
    previous=digest((source.session_uid,base_fields,points,polys,common_inputs))
    for key,name in ((35,'plaster'),(36,'pits'),(37,'metal'),(38,'metal_tear'),(39,'clay')):
        keys[key]=digest((previous,key,encoded[name]));previous=keys[key]
    keys[40]=digest((keys[39],encoded['cellf'],inputs.get('cellf_strokes')))
    params.incremental_keys=keys


def reset():
    from . import build,input_prep,background_compute,surface_guidance
    clear_outputs();surface_guidance.clear()
    build._CACHES.clear();input_prep.clear_prep_cache()
    background_compute.clear_intermediates()


def settings_changed(self,context):
                                                                                  
    reset()
    from .core_loader import _redraw_interface
    _redraw_interface(self,context)

                                                                              
                                                                               
_outputs={}

def clear_outputs():
    from . import boolean_break
    _outputs.clear();_bodies.clear();_slots.clear()
    boolean_break.clear_split_plans()

def geometry_arrays(mesh):
    co=np.empty(len(mesh.vertices)*3,np.float32);mesh.vertices.foreach_get('co',co)
    edges=np.empty(len(mesh.edges)*2,np.int32);mesh.edges.foreach_get('vertices',edges)
    vi=np.empty(len(mesh.loops),np.int32);mesh.loops.foreach_get('vertex_index',vi)
    counts=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('loop_total',counts)
    return co, edges, vi, counts


def mesh_state(mesh,appearance=True,materials=True):
    # appearance=False: shape only. Material slots, face material indices and UV maps
    # are left out, so a material swap or Quick UV on an output is not a new shape.
    co, edges, vi, counts = geometry_arrays(mesh)
    mats=np.empty(len(mesh.polygons),np.int32);mesh.polygons.foreach_get('material_index',mats)
    attrs={}
    for attr in mesh.attributes:
        if attr.name=='position':continue
        if not appearance and (attr.name=='material_index' or (attr.domain=='CORNER' and attr.data_type=='FLOAT2') or attr.name.startswith('.')):continue
        values=[]
        if len(attr.data):
            item=attr.data[0]
            field=next((item.bl_rna.properties.get(n) for n in ('value','vector','color') if item.bl_rna.properties.get(n)),None)
            if field is not None:
                if field.type in {'FLOAT','INT','BOOLEAN'}:
                    width=field.array_length if field.is_array else 1
                    values=np.empty(len(attr.data)*width,np.float32 if field.type=='FLOAT' else np.int32)
                    attr.data.foreach_get(field.identifier,values)
                elif field.type=='STRING':values=[getattr(d,field.identifier) for d in attr.data]
        attrs[attr.name]=(attr.domain,attr.data_type,values)
    uv={}
    for layer in mesh.uv_layers:
        a=np.empty(len(mesh.loops)*2,np.float32);layer.data.foreach_get('uv',a);uv[layer.name]=a
    uv_selection=(mesh.uv_layers.active.name if mesh.uv_layers.active else None,
                  tuple(layer.name for layer in mesh.uv_layers if layer.active_render))
    if not appearance:return digest((co,edges,vi,counts,attrs))
    if not materials:return digest((co,edges,vi,counts,mats,attrs,uv,uv_selection,len(mesh.materials)))
    return digest((co,edges,vi,counts,mats,attrs,uv,uv_selection,[(m.session_uid,m.name_full) if m else None for m in mesh.materials]))

def _state(context,source,value,_seen=None):
    seen=set() if _seen is None else _seen
    tracked=isinstance(value,(bpy.types.ID,bpy.types.PropertyGroup,bpy.types.Modifier))
    identity=value.as_pointer() if tracked else None
    if identity in seen:return ('REFERENCE',identity)
    if tracked:seen=seen|{identity}
    def state(item):return _state(context,source,item,seen)
    if value is None or isinstance(value,(str,bool,int,float)):return _resources(value)
    if isinstance(value,set):return sorted(state(v) for v in value)
    if isinstance(value,dict) or type(value).__name__=='IDPropertyGroup':return {k:state(v) for k,v in value.items()}
    if isinstance(value,bpy.types.Image):
        # A saved, unedited image file is identified by its file; reading every
        # pixel (hundreds of MB for a 4K map) is kept for painted or generated images.
        if value.source in {'FILE','SEQUENCE','MOVIE','TILED'} and not value.is_dirty and value.packed_file is None and value.filepath:
            path=bpy.path.abspath(value.filepath,library=value.library)
            try:
                stat=os.stat(path)
                return (value.session_uid,value.name_full,'FILE',path,stat.st_mtime_ns,stat.st_size,tuple(value.size),
                        value.colorspace_settings.name,value.alpha_mode)
            except OSError:pass
        pixels=np.empty(len(value.pixels),np.float32)
        if pixels.size:value.pixels.foreach_get(pixels)
        return (value.session_uid,value.name_full,digest(pixels))
    if isinstance(value,bpy.types.Texture):
        fields={p.identifier:state(getattr(value,p.identifier)) for p in value.bl_rna.properties
                if not p.is_readonly and p.identifier not in {'rna_type','name','id_data'} and p.type in {'BOOLEAN','INT','FLOAT','STRING','ENUM'}}
        return (value.session_uid,fields,state(getattr(value,'image',None)))
    if isinstance(value,bpy.types.Object):
        geometry=None
        if value.type=='MESH':geometry=mesh_state(value.data)
        elif value.type in {'CURVE','GREASEPENCIL','GPENCIL'}:
            from . import paint
            geometry=paint.read_paths(context,source,value,min_points=1)
        modifiers=[state(m) for m in value.modifiers if m.show_viewport]
        evaluated=None
        if value.type=='MESH' and modifiers:
            dg=context.evaluated_depsgraph_get()
            obj=value.evaluated_get(dg)
            try:
                mesh=obj.to_mesh()
                if mesh is not None:evaluated=mesh_state(mesh)
            finally:obj.to_mesh_clear()
        return (value.session_uid,value.type,tuple(tuple(row) for row in value.matrix_world),geometry,
                modifiers,evaluated,[state(child) for child in sorted(value.children,key=lambda o:o.session_uid)])
    if isinstance(value,bpy.types.Collection):
        return [state(o) for o in sorted(value.all_objects,key=lambda o:o.name_full)]
    if isinstance(value,bpy.types.ID):return (value.bl_rna.identifier,value.name_full)
    if isinstance(value,(bpy.types.PropertyGroup,bpy.types.Modifier)):
        values={p.identifier:state(getattr(value,p.identifier)) for p in value.bl_rna.properties
                if p.identifier not in {'rna_type','name','result','execution_time','is_active','id_data'}}
        if isinstance(value,bpy.types.Modifier):
            try:inputs=dict(value.items())
            except TypeError:inputs={}
            values['_inputs']=state(inputs)
        return values
    try:items=iter(value)
    except TypeError:return str(value)
    return [state(v) for v in items]

_PREFIXES={'pipes':('pipe_','boolean_pipe_','rebar_','break_','concrete_','ui_boolean_enable','boolean_generated_enable'),
           'cables':('cable_','boolean_cable_','rebar_','break_','concrete_','ui_boolean_enable','boolean_generated_enable'),
           'surface_rebar':('surf_','surface_rebar_')}

def output_key(context,source,target,key,settings=None,*,output=None):
    from .params import unit_factor
    if not enabled() or target is None:return None
    p=source.project_damage
    if settings is not None:values=_state(context,source,settings)
    else:
        prefixes=_PREFIXES.get(key,(key+'_',))
        values={r.identifier:_state(context,source,getattr(p,r.identifier)) for r in p.bl_rna.properties
                if r.identifier.startswith(prefixes)}
    if key == 'gen_metal' and output is not None and settings is not None:
        from .metal_tear_gen import blast_rows
        values['blast_points'] = blast_rows(output, settings)
    if key=='weld':
        values={**values,'drawing':_state(context,source,p.weld_drawing)}
    if key=='surface_rebar':values={**values,'effective_rebar_resolution':preview_quality.rebar_resolution(p,'surface_rebar_resolution')}
    return digest((source.session_uid,key,mesh_state(source.data,False),mesh_state(target.data,False),values,
                   p.global_scale,preview_quality.percent(p),p.seed,p.all_off,unit_factor(context),
                   tuple(tuple(row) for row in source.matrix_world),
                   tuple(tuple(row) for row in target.matrix_world),
                   [_state(context,source,m) for m in source.modifiers if m.show_viewport],
                   [_state(context,source,m) for m in target.modifiers if m.show_viewport],
                   context.scene.frame_current if len(source.modifiers)+len(target.modifiers) else 0,
                   ([g.name for g in source.vertex_groups],[(v.index,g.group,g.weight) for v in source.data.vertices for g in v.groups]) if source.vertex_groups else [],
                   preference('fracture_surface_proxy',False)))

def reuse_output(context,source,target,key,obj,settings=None):
    if not enabled() or obj is None:return False
    if any(m.show_viewport and m.type!='WEIGHTED_NORMAL' for o in (source,target) if o is not None for m in o.modifiers):return False
    entry=_outputs.get((source.session_uid,key))
    if entry is None:return False
    try:
        return entry==(output_key(context,source,target,key,settings,output=obj),obj.session_uid,mesh_state(obj.data,False))
    except ReferenceError:return False

def remember_output(context,source,target,key,obj,settings=None):
    if not enabled() or obj is None:return
    token=output_key(context,source,target,key,settings,output=obj)
    _outputs[(source.session_uid,key)]=(token,obj.session_uid,mesh_state(obj.data,False))
    while len(_outputs)>64:_outputs.pop(next(iter(_outputs)))

_bodies={}
_STANDALONE_ENABLE={'grass_enable','gravel_enable','foam_enable','weld_enable','ivy_enable','roots_enable'}
_BODY_UI={'background_compute','live_update','defer_ms','ui_settings_tab','rna_type','name'}

def body_key(context,source):
    p=source.project_damage
    settings={r.identifier:_state(context,source,getattr(p,r.identifier)) for r in p.bl_rna.properties
              if r.identifier not in _BODY_UI|_STANDALONE_ENABLE and
              (not r.identifier.startswith('ui_') or r.identifier=='ui_boolean_enable')}
    from . import fracture_stack
    for name in tuple(settings):
        if name.startswith(('fract_','fracture_')) or name=='guides':settings.pop(name)
    settings['effective_rebar_resolutions']=[preview_quality.rebar_resolution(p,name) for name in ('rebar_resolution','concrete_rebar_resolution','hang_rebar_resolution','surface_rebar_resolution')]
    settings['fracture_stack']=[(key,{name:_state(context,source,value) for name,value in values.items()})
                               for key,values in fracture_stack.state(source)]
    from . import wall_stack
    if wall_stack.SERIAL in source:
        settings['wall_layers'] = wall_stack.fingerprint(context, source)
    # The link records the object, but Break Surface settings live on the cutter.
    breaker_settings = [_state(context, source, row.obj.project_damage_breaker)
                        if row.obj is not None else None for row in p.breakers]
    return digest((source.session_uid,mesh_state(source.data),settings,preview_quality.percent(p),breaker_settings,
                   _state(context,source,source.project_damage_input),
                   _state(context,source,source.project_damage_paper) if p.paper_generator_enable and not p.all_off else None,
                   tuple(tuple(row) for row in source.matrix_world),
                   [_state(context,source,m) for m in source.modifiers if m.show_viewport],
                   context.scene.frame_current,context.scene.unit_settings.scale_length,
                   ([g.name for g in source.vertex_groups],[(v.index,g.group,g.weight) for v in source.data.vertices for g in v.groups]) if source.vertex_groups else [],
                   preference('fracture_surface_proxy',False)))

def body_unchanged(context,source,result):
    if not enabled() or result is None:return False
    if any(m.show_viewport for m in source.modifiers):return False
    return _bodies.get(source.session_uid)==(body_key(context,source),result.session_uid,mesh_state(result.data,False))

def remember_body(context,source,result):
    if not enabled() or result is None:return
    _bodies[source.session_uid]=(body_key(context,source),result.session_uid,mesh_state(result.data,False))
    while len(_bodies)>16:_bodies.pop(next(iter(_bodies)))


_slots={}

def slot_signature(obj):
    return tuple(slot.material.session_uid if slot.material is not None else None for slot in obj.material_slots)

def remember_slots(source):
    # Kept without the cache preferences: it only decides whether a source edit was a material swap.
    try:_slots[source.session_uid]=(slot_signature(source),mesh_state(source.data,materials=False))
    except (ReferenceError,RuntimeError):return
    while len(_slots)>32:_slots.pop(next(iter(_slots)))

def material_swaps(source):
    """(old uid, new material) per changed slot when a source update changed nothing
    but the materials in its existing slots; None when a rebuild is needed."""
    saved=_slots.get(source.session_uid)
    if saved is None or source.mode!='OBJECT':return None
    current=slot_signature(source)
    if current==saved[0] or len(current)!=len(saved[0]):return None
    if mesh_state(source.data,materials=False)!=saved[1]:return None
    swaps=[(old,slot.material) for old,new,slot in zip(saved[0],current,source.material_slots) if old!=new]
    if any(old is None or material is None for old,material in swaps):return None
    return swaps


def body_known(source):return source.session_uid in _bodies


def invalidate_body(source):
    _bodies.pop(source.session_uid,None)
