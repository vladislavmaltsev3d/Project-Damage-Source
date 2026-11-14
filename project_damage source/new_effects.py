# передаём настройки рисования и формы эффектов в расчёт
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host inputs for hits, wood, lath, wallpaper and drawn fabric tears."""
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from . import ownership

DRAWINGS = ('draw_mask_drawing','chips_drawing','rebar_drawing','bevel_drawing','scratch_drawing','wood_drawing', 'lath_drawing', 'wallpaper_drawing',
            'fabric_holes_drawing', 'fabric_edges_drawing', 'bullet_drawing',
            'ground_drawing', 'brick_drawing', 'fissure_drawing',
            'glass_holes_drawing', 'glass_impacts_drawing')
DRAWINGS += ('rbf_cuts_drawing','rbf_impacts_drawing')
DRAWINGS += ('fract_helper_drawing',)
DRAWINGS += ('cellf_drawing','cellf_surface_drawing','cellf_guidance_drawing','rusty_drawing',)
DRAWINGS += ('corner_drawing','clay_drawing')
DRAWINGS += ('build_drawing','peeling_drawing','metaltear_drawing')
DRAWINGS += ('weld_drawing','grass_drawing','gravel_drawing')


def fill_hit_empties(context, source, core, factor, module='hit'):
    p = source.project_damage
    collection = getattr(p, module + '_collection')
    active = getattr(p, ('metal' if module == 'metal_crush' else module) + '_enable')
    if module=='hit':active = active or p.hit_through_enable
    if p.all_off or not active or collection is None:
        return []
    deps = context.evaluated_depsgraph_get()
    inverse = source.evaluated_get(deps).matrix_world.inverted_safe()
    result = []
    for obj in sorted(collection.all_objects, key=lambda o: o.name):
        if obj.type != 'EMPTY' or obj == source:
            continue
        matrix = inverse @ obj.evaluated_get(deps).matrix_world
        entry = core.MetalCrushEmpty() if module == 'metal_crush' else core.BuildingBlastEmpty() if module == 'blast' else core.ConcreteHitEmpty()
        entry.rel = ([matrix.translation[i] * factor for i in range(3)]
                     + [matrix.col[c][r] for c in range(3) for r in range(3)])
        entry.size = obj.empty_display_size * factor
        entry.strength = max(0.0, float(obj.get('project_damage_' + ('metal' if module == 'metal_crush' else module) + '_strength', 1.0)))
        if not np.isfinite(entry.size) or not np.isfinite(entry.strength):
            raise ValueError('Impact size and strength must be finite')
        result.append(entry)
    return result


def fill_blast_empties(context, source, core, factor):
    return fill_hit_empties(context, source, core, factor, 'blast')


def collect(context, source, points, polys, factor):
    from . import paint
    p = source.project_damage
    inputs, reports = {}, []
    if p.all_off:
        return inputs, reports
    cell_fracture=(p.fract_through_enable and p.fract_through) or (p.fract_enable and p.fract_physics)
    if cell_fracture and p.fract_helper_drawing is not None:
        inputs['fract_helper_strokes']=[np.ascontiguousarray(path*factor,dtype=np.float64)
            for path in paint.read_paths(context,source,p.fract_helper_drawing,min_points=2)]
        if not inputs['fract_helper_strokes']:
            reports.append(({'WARNING'},'Fracture Helper: no visible strokes at this frame; helper ignored'))
    if p.rbf_enable:
        for name,minimum in (('cuts',2),('impacts',1)):
            inputs['rbf_cut_strokes' if name=='cuts' else 'rbf_impact_strokes']=[
                np.ascontiguousarray(path*factor,dtype=np.float64)
                for path in paint.read_paths(context,source,getattr(p,'rbf_'+name+'_drawing'),min_points=minimum)]
                                                                           
    if p.cellf_enable and p.cellf_guidance_drawing is not None and p.cellf_guidance_amount > 0:
        inputs['cellf_strokes']=[np.ascontiguousarray(path*factor,dtype=np.float64)
            for path in paint.read_cell_paths(context,source,'GUIDANCE')]
        if not inputs['cellf_strokes'] and not p.cellf_guidance_drawing.get('project_damage_cell_layers'):
            reports.append(({'WARNING'}, 'Cell Fracture Guidance: no strokes at the current frame'))
    if p.draw_mask_enable:
        from .params import _clamp_scale
        paths = [np.ascontiguousarray(path*factor, dtype=np.float64)
                 for path in paint.read_paths(context, source, p.draw_mask_drawing)]
        if paths:
            inputs.update(draw_mask_paths=paths,
                          draw_mask_radius=p.draw_mask_radius*_clamp_scale(p.global_scale/100.0),
                          draw_mask_soft=p.draw_mask_soft/100.0,
                          draw_mask_invert=p.draw_mask_mode=='1',
                          draw_mask_size=p.draw_mask_size/100.0)
        else:
            reports.append(({'WARNING'}, 'Draw Mask: no visible strokes at this frame; mask ignored'))
    if p.chips_enable and p.chips_mode=='PAINT':
        from .params import _clamp_scale
        inputs['chip_strokes']=[np.ascontiguousarray(path*factor,dtype=np.float64)
                               for path in paint.read_paths(context,source,p.chips_drawing)]
        inputs['chip_paint_radius']=p.chips_paint_radius*_clamp_scale(p.global_scale/100.0)
        if not inputs['chip_strokes']:
            reports.append(({'WARNING'},'Edge Draw: no visible strokes at this frame; no edge chips generated'))
    for module in ('scratch','wood', 'lath', 'bullet', 'ground', 'brick', 'fissure', 'rusty'):
        on = (getattr(p, module + '_enable')
              or (module == 'lath' and (p.lath_v2_enable or p.lath_riven_enable or p.lath_framing_enable))
              or (module == 'brick' and p.brick_v2_enable))
        if on and (module in {'bevel','scratch','fissure'} or int(getattr(p, module + '_mode')) == 1 or (module in {'wood','bullet','ground','rusty'} and getattr(p,module+'_mode')=='2')):
            paths = [np.ascontiguousarray(path * factor) for path in
                     paint.read_paths(context, source, getattr(p, module + '_drawing'), min_points=1 if module in {'bullet','rusty'} else 2)]
            if module in {'wood','bullet','ground','rusty'} and getattr(p,module+'_mode')=='2':
                from . import damage_proximity
                paths=damage_proximity.paths(context,source,module,points,polys,factor)
            inputs[module + '_strokes'] = paths
            if not paths and (module != 'bevel' or p.bevel_drawn_only):
                reports.append(({'WARNING'}, module.title() + (' Proximity: no point touches the source' if getattr(p,module+'_mode','0')=='2' else ' Paint: no strokes at this frame')))
    from . import damaged_bevel
    damaged_bevel.collect(context, source, factor, inputs, reports)
    for key, drawing_prop, enabled in (
            ('wallpaper', 'wallpaper_drawing', p.wallpaper_enable and p.wallpaper_paint_enable),
            ('fabric_hole', 'fabric_holes_drawing', (p.fabric_enable or p.fabric_solid_enable) and p.fabric_paint_holes_enable),
            ('fabric_edge', 'fabric_edges_drawing', (p.fabric_enable or p.fabric_solid_enable) and p.fabric_paint_edges_enable)):
        if not enabled:
            continue
        drawing = getattr(p, drawing_prop)
        paths, normals = paint.read_regions(context, source, drawing, points, polys, factor, reports=reports,
                                            label=key.replace('_', ' ').title() + ' contour')
        if key == 'wallpaper':
            inputs['wallpaper_paths'], inputs['wallpaper_normals'] = paths, normals
        else:
            inputs['fabric_holes' if key == 'fabric_hole' else 'fabric_edges'] = paths
            inputs[key + '_normals'] = normals
        if drawing is not None and not paths:
            reports.append(({'WARNING'}, key.replace('_', ' ').title() + ': no usable closed contours'))
    if p.glass_enable or p.glass_through_enable or p.glass_hit_enable or p.glass_drawn_enable:
        if p.glass_hit_enable:
            paths, normals = [], None
        else:
            paths, normals = paint.read_regions(context, source, p.glass_holes_drawing, points, polys, factor,
                                                glass=p.glass_drawn_enable)
        inputs['glass_holes'], inputs['glass_hole_normals'] = paths, normals
        if p.glass_hit_enable or p.glass_drawn_enable:
            paths, normals = [], None
        else:
            paths, normals = paint.read_stroke_regions(context, source, p.glass_impacts_drawing, points, polys, factor)
        if p.glass_hit_enable and p.glass_hit_collection is not None:
                                                                                  
            paths = list(paths) if paths is not None else []
            normals = [tuple(n) for n in normals] if normals is not None and len(normals) else []
            faces = [tuple(int(v) for v in row[:3]) if row[2] == row[3] else tuple(int(v) for v in row) for row in polys]
            bvh = BVHTree.FromPolygons([Vector(v) for v in points], faces, all_triangles=False)
            deps = context.evaluated_depsgraph_get()
            inverse = source.evaluated_get(deps).matrix_world.inverted_safe()
            for obj in sorted(p.glass_hit_collection.all_objects, key=lambda o: o.name):
                if obj.type != 'EMPTY' or obj == source:
                    continue
                local = inverse @ obj.evaluated_get(deps).matrix_world.translation
                hit, normal, face, distance = bvh.find_nearest(Vector([local[i] * factor for i in range(3)]))
                if hit is None:
                    continue
                paths.append(np.ascontiguousarray([[hit.x, hit.y, hit.z]], dtype=np.float64))
                normals.append(tuple(normal))
            normals = np.ascontiguousarray(normals, dtype=np.float64).reshape(-1, 3) if normals else None
        inputs['glass_impacts'], inputs['glass_impact_normals'] = paths, normals
        if p.glass_hit_enable and not paths:
            reports.append(({'WARNING'}, 'Hit Glass: select a collection containing hit empties'))
        if p.glass_drawn_enable and not len(inputs['glass_holes']):
            reports.append(({'WARNING'}, 'Glass Drawn: draw a closed contour in Holes first'))
    return inputs, reports


def hash_inputs(hasher, params, inputs):
                                                                                   
    import struct
    hasher.update(b'rbf_impacts')
    hasher.update(struct.pack('<Q',len(params.rbf.impacts)))
    for im in params.rbf.impacts:
        hasher.update(struct.pack('<5d',im.x,im.y,im.z,im.radius,im.strength))
    cellf = getattr(params, 'cellf', None)
    if cellf is not None and cellf.enable:
        hasher.update(b'cellf_proximity')
        hasher.update(struct.pack('<Q', len(cellf.proximity)))
        for q in cellf.proximity:
            hasher.update(struct.pack('<5d', q.x, q.y, q.z, q.radius, q.strength))
    for key, value in sorted(inputs.items()):
        hasher.update(key.encode())
        values = value if isinstance(value, list) else ([] if value is None else [value])
        hasher.update(struct.pack('<Q', len(values)))
        for array in values:
            array = np.ascontiguousarray(array, dtype=np.float64)
            hasher.update(struct.pack('<Q', array.size))
            hasher.update(array.tobytes())
    hasher.update(b'metal')
    hasher.update(struct.pack('<Q', len(params.metal.impactors)))
    for im in params.metal.impactors:
        for data, dtype in ((im.pts,np.float64),(im.tris,np.int32)):
            data=np.asarray(data,dtype=dtype); hasher.update(struct.pack('<Q',data.size)); hasher.update(data.tobytes())
        hasher.update(struct.pack('<d',im.strength))
    hasher.update(struct.pack('<Q',len(params.metal.empties)))
    for e in params.metal.empties:
        hasher.update(np.asarray(list(e.rel)+[e.size,e.strength],dtype=np.float64).tobytes())
    for entry in params.hit.empties:
        hasher.update(np.asarray(list(entry.rel) + [entry.size, entry.strength], dtype=np.float64).tobytes())
    hasher.update(b'blast')
    for entry in params.blast.empties:
        hasher.update(np.asarray(list(entry.rel) + [entry.size, entry.strength], dtype=np.float64).tobytes())


# действие «Apply Effect»
class PROJECT_DAMAGE_OT_apply_effect(bpy.types.Operator):
    bl_idname = 'project_damage.apply_effect'
    bl_label = 'Apply Effect'
    bl_options = {'REGISTER', 'UNDO'}
    module: bpy.props.StringProperty()

    @classmethod
    def poll(cls, context):
        from .build import PROJECT_DAMAGE_OT_update
        return PROJECT_DAMAGE_OT_update.poll(context)

    def execute(self, context):
        from . import build
        from .effect_schema import EFFECT_FIELDS
        if self.module not in EFFECT_FIELDS:
            return {'CANCELLED'}
        return build._apply_extra(context, self, self.module)


# действие «Add Impact Empty»
class PROJECT_DAMAGE_OT_add_hit(bpy.types.Operator):
    bl_idname = 'project_damage.add_hit'
    bl_label = 'Add Impact Empty'
    bl_options = {'REGISTER', 'UNDO'}
    module: bpy.props.EnumProperty(items=(('hit', 'Impact', ''), ('blast', 'Blast', ''), ('metal_crush','Metal Crush','')), default='hit')

    @classmethod
    def poll(cls, context):
        source = ownership.context_source(context)
        return source is not None and source.type == 'MESH' and context.mode == 'OBJECT'

    def execute(self, context):
        from . import params
        source = ownership.context_source(context)
        p = source.project_damage
        prop = self.module + '_collection'
        collection = getattr(p, prop)
        if collection is None:
            collection = bpy.data.collections.new(source.name + '_project_damage_Impacts')
            context.scene.collection.children.link(collection)
            setattr(p, prop, collection)
        empty = bpy.data.objects.new(source.name + '_Impact', None)
        collection.objects.link(empty)
        empty.empty_display_type = 'SPHERE'
        empty.empty_display_size = (300.0 if self.module == 'blast' else 20.0) / params.unit_factor(context)
        empty.location = context.scene.cursor.location
        empty['project_damage_' + self.module + '_strength'] = 1.0
        if not (self.module=='hit' and p.hit_through_enable):
            setattr(p, ('metal' if self.module == 'metal_crush' else self.module) + '_enable', True)
        if self.module == 'metal_crush': p.metal_style = '1'
        self.report({'INFO'}, 'Impact placed at the 3D cursor; move it close to the surface')
        return {'FINISHED'}


# действие «Load Material Defaults»
class PROJECT_DAMAGE_OT_bullet_preset(bpy.types.Operator):
    bl_idname = 'project_damage.bullet_preset'
    bl_label = 'Load Material Defaults'
    bl_options = {'REGISTER', 'UNDO'}
    poll = PROJECT_DAMAGE_OT_add_hit.poll

    def execute(self, context):
        from . import core_loader, params
        from .effect_schema import EFFECT_FIELDS
        source = ownership.context_source(context)
        core = core_loader.load()
        p = params.fill_params(source.project_damage, core)
        core.bullet_impact_preset(p.bullet, int(source.project_damage.bullet_material))
        scale = params._clamp_scale(source.project_damage.global_scale / 100.0)
        for name, is_length in EFFECT_FIELDS['bullet']:
            value = getattr(p.bullet, name)
            if is_length: value /= max(scale, 1e-9)
            if name in {'mode', 'material', 'shot_mode', 'grain_mode'}: value = str(value)
            setattr(source.project_damage, 'bullet_' + name, value)
        return {'FINISHED'}


def fill_metal_impactors(context, source, core, factor):
    p=source.project_damage
    if p.all_off or not p.metal_enable or p.metal_style != '0' or p.metal_collection is None: return []
    deps=context.evaluated_depsgraph_get()
    inverse=source.evaluated_get(deps).matrix_world.inverted_safe()
    result=[]
    for ob in sorted(p.metal_collection.all_objects,key=lambda ob:ob.name):
        if ob == source or ob.get('project_damage_result_of') or ob.type not in {'MESH','CURVE','SURFACE','META','FONT'}: continue
        ev=ob.evaluated_get(deps); mesh=ev.to_mesh()
        if mesh is None: continue
        try:
            mesh.calc_loop_triangles()
            co=np.empty(len(mesh.vertices)*3,dtype=np.float64); mesh.vertices.foreach_get('co',co)
            matrix=np.array(inverse @ ev.matrix_world)
            co=(co.reshape(-1,3) @ matrix[:3,:3].T + matrix[:3,3])*factor
            tris=np.empty(len(mesh.loop_triangles)*3,dtype=np.int32); mesh.loop_triangles.foreach_get('vertices',tris)
        finally: ev.to_mesh_clear()
        if not len(tris): continue
        im=core.MetalDeformImpactor(); im.set_mesh(np.ascontiguousarray(co),np.ascontiguousarray(tris.reshape(-1,3)))
        im.strength=max(0.0,float(ob.get('project_damage_metal_strength',1.0)))
        if not np.isfinite(im.strength): raise ValueError('Metal impact strength must be finite')
        result.append(im)
    return result


# действие «Load Style Defaults»
class PROJECT_DAMAGE_OT_recipe_defaults(bpy.types.Operator):
    bl_idname='project_damage.recipe_defaults'
    bl_label='Load Style Defaults'
    bl_options={'REGISTER','UNDO'}
    module: bpy.props.EnumProperty(items=(('bevel','Bevel',''),('scratch','Scratches','')))
    poll=PROJECT_DAMAGE_OT_add_hit.poll
    def execute(self,context):
        from . import core_loader
        from .effect_schema import EFFECT_FIELDS
        p=ownership.context_source(context).project_damage; core=core_loader.load()
        block=core.EdgeBevelParams() if self.module=='bevel' else core.ScratchDrawParams()
        choice=int(p.bevel_preset if self.module=='bevel' else p.scratch_style)
        if self.module=='bevel' and choice==0: return {'FINISHED'}
        (core.edge_bevel_preset if self.module=='bevel' else core.scratch_draw_preset)(block,choice)
        enums={'profile','size_mode','style'}
        for name,_ in EFFECT_FIELDS[self.module]:
            if name in {'enable','seed','avoid'}: continue
            if self.module=='scratch' and name in {'density','max_scratches','paint_snap'}: continue
            if self.module=='bevel' and name not in {'size_mode','size_top','size_side','taper','fillet_top','fillet_side','roundness','max_share','profile'}: continue
            value=getattr(block,name)
            setattr(p,self.module+'_'+name,str(value) if name in enums else value)
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in (PROJECT_DAMAGE_OT_apply_effect, PROJECT_DAMAGE_OT_add_hit, PROJECT_DAMAGE_OT_bullet_preset, PROJECT_DAMAGE_OT_recipe_defaults):
        bpy.utils.register_class(cls)


# убираем подключения модуля при выключении аддона
def unregister():
    for cls in (PROJECT_DAMAGE_OT_recipe_defaults, PROJECT_DAMAGE_OT_bullet_preset, PROJECT_DAMAGE_OT_add_hit, PROJECT_DAMAGE_OT_apply_effect):
        bpy.utils.unregister_class(cls)
