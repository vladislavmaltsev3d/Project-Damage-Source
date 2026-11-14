# настройки, материалы и результат арматурного заполнения
# SPDX-License-Identifier: GPL-3.0-or-later
from . import output_collections
from .blender_compat import enable_nodes
import bpy
import numpy as np
from . import live, params
REBAR_STAMP="project_damage_rebar_of"
FIELDS = (('enable', 'bool', False, False), ('seed', 'int32_t', 0, False), ('layout', 'int32_t', 0, False), ('axis', 'int32_t', 0, False), ('side', 'int32_t', 0, False), ('angle', 'double', 0.0, False), ('cover', 'double', 3.0, True), ('spacing_u', 'double', 15.0, True), ('spacing_v', 'double', 15.0, True), ('bars_u', 'bool', True, False), ('bars_v', 'bool', True, False), ('outer', 'int32_t', 0, False), ('amount', 'double', 1.0, False), ('jitter', 'double', 0.08, False), ('tie_spacing', 'double', 20.0, True), ('tie_radius', 'double', 0.4, True), ('spiral', 'bool', False, False), ('type', 'int32_t', 0, False), ('radius', 'double', 0.6, True), ('scale', 'double', 1.0, False), ('rib_height', 'double', 0.12, False), ('rib_pitch', 'double', 0.65, False), ('twist_pitch', 'double', 10.0, False), ('sides', 'int32_t', 12, False), ('detail', 'double', 1.0, False), ('embed', 'double', 6.0, True), ('min_exposed', 'double', 1.0, False), ('wobble', 'double', 0.15, False), ('bow', 'double', 0.3, False), ('bend', 'double', 0.5, False), ('broken', 'double', 0.3, False), ('trim', 'double', 0.35, False), ('neck', 'double', 0.5, False), ('drop_loose', 'bool', True, False), ('mode', 'int32_t', 0, False), ('fit', 'int32_t', 0, False), ('deform', 'double', 0.0, False), ('reach', 'double', 0.0, True), ('core', 'bool', False, False), ('show_all', 'bool', False, False), ('stroke_radius', 'double', 1.0, False), ('stroke_embed', 'double', 1.0, False))
FIELDS += (('material', 'int32_t', 0, False), ('brick_length', 'double', 25.0, True), ('brick_height', 'double', 6.5, True), ('brick_depth', 'double', 12.0, True), ('mortar', 'double', 1.0, True), ('missing', 'double', 0.3, False), ('brick_broken', 'double', 0.3, False), ('chamfer', 'double', 0.12, False), ('brick_jitter', 'double', 0.15, False), ('plank_width', 'double', 12.0, True), ('plank_thickness', 'double', 2.5, True), ('plank_gap', 'double', 0.6, True), ('plank_dir', 'int32_t', 0, False), ('splinter', 'double', 0.6, False))
REBAR_FILL_ITEMS=(('0','Rebar','Steel reinforcement'),('1','Bricks','One connected brick layer held by the remaining solid'),('2','Wood Planks','Boards with splintered broken ends'))
from . import framing as _framing
FIELDS += _framing.FIELDS
REBAR_FILL_ITEMS += (_framing.FILL_ITEM,)
from . import chips as _chips
FIELDS += _chips.FIELDS
REBAR_FILL_ITEMS += (_chips.FILL_ITEM,)
REBAR_FILL_ITEMS += (('5','Wood Laths','Thin wooden laths behind the surface'),)
REBAR_PLANK_DIR_ITEMS=(('0','Horizontal','Boards along the horizontal family'),('1','Vertical','Vertical boards or staves'))
FIELDS += (('mortar_joints','bool',True,False),('mortar_recess','double',.08,False))
FIELDS += (('brick_3d','bool',False,False),)
REBAR_TYPE_ITEMS = (
    ('0', "Ribbed", "Deformed bar: two longitudinal ribs and inclined crescent ribs"),
    ('1', "Helical Ribs", "Cold-rolled bar with one continuous helical rib"),
    ('2', "Smooth", "Plain round bar, old structures and wire mesh"),
    ('3', "Twisted Square", "Cold-twisted square bar"),
    ('4', "Strand", "7-wire prestressing strand"),
)

REBAR_LAYOUT_ITEMS = (
    ('0', "Grid", "One mesh layer (wall, slab)"),
    ('1', "Double Grid", "A mesh layer behind each face"),
    ('2', "Column Cage", "Corner and side bars with stirrups"),
    ('3', "Round Cage", "Bars on a circle with hoops or a spiral"),
)

REBAR_AXIS_ITEMS = (
    ('0', "Auto", "Grid: thinnest side is the wall normal. Cage: longest side is the column axis"),
    ('1', "X", ""), ('2', "Y", ""), ('3', "Z", ""),
)

REBAR_SIDE_ITEMS = (
    ('0', "+ Face", "Near the face on the positive side of the axis"),
    ('1', "- Face", "Near the face on the negative side of the axis"),
    ('2', "Centre", "In the middle of the wall"),
)

REBAR_OUTER_ITEMS = (
    ('0', "U Bars", "U bars sit nearer the surface"),
    ('1', "V Bars", "V bars sit nearer the surface"),
)

REBAR_PRESETS = {
    '1': dict(mode='0', fit='0', reach=0.0, layout='0', type='0', radius=0.6, spacing_u=15.0, spacing_v=15.0, cover=3.0, rib_height=0.12),                     
    '2': dict(mode='0', fit='0', reach=0.0, layout='1', type='0', radius=0.8, spacing_u=20.0, spacing_v=20.0, cover=3.5, rib_height=0.12),                       
    '3': dict(mode='0', fit='0', reach=0.0, layout='2', type='0', radius=1.0, spacing_u=15.0, cover=3.0, tie_spacing=15.0, tie_radius=0.4),                      
    '4': dict(mode='0', fit='0', reach=0.0, layout='3', type='0', radius=0.8, spacing_u=12.0, cover=3.0, tie_spacing=8.0, tie_radius=0.35, spiral=True),                
    '5': dict(mode='0', fit='0', reach=0.0, layout='0', type='2', radius=0.25, spacing_u=10.0, spacing_v=10.0, cover=1.5, rib_height=0.0, bend=0.7, broken=0.5),             
    '6': dict(mode='0', fit='0', reach=0.0, layout='0', type='2', radius=0.8, spacing_u=25.0, spacing_v=30.0, cover=2.0, wobble=0.3),                                
    '7': dict(mode='0', fit='0', reach=0.0, layout='0', type='3', radius=0.6, spacing_u=18.0, spacing_v=18.0, cover=3.0),                                           
    '8': dict(mode='0', fit='0', reach=0.0, layout='0', type='4', radius=0.65, spacing_u=40.0, spacing_v=120.0, bars_v=False, cover=5.0,
              twist_pitch=8.0, broken=0.15, bend=0.8),                                                                                
}
REBAR_PRESETS['9'] = dict(mode='1', fit='2', layout='2', type='0', radius=0.4, spacing_u=8.0, cover=2.0,
                          tie_spacing=12.0, tie_radius=0.25, core=False, reach=20.0)
REBAR_PRESETS['10'] = dict(mode='0', fit='2', reach=0.0, layout='0', type='2', radius=0.3, spacing_u=6.0, spacing_v=6.0,
                           cover=1.5, rib_height=0.0, bend=0.6, broken=0.4)

REBAR_PRESET_ITEMS = (
    ('0', "Custom", "Keep the fields as they are"),
    ('1', "Wall Mesh", ""), ('2', "Double Wall", ""), ('3', "Column Cage", ""),
    ('4', "Round Column", ""), ('5', "Wire Mesh", ""), ('6', "Old Smooth Bars", ""),
    ('7', "Twisted Square", ""), ('8', "Prestress Strands", ""),
    ('9', "Statue Armature", "Built-in cage that follows the body, revealed by the damage"),
    ('10', "Sculpture Mesh", "Fine wire mesh under the damaged surface of a sculpt"),
)

def _apply_rebar_preset(self, context):
    values = REBAR_PRESETS.get(self.rebar_preset)
    if values:
        for key, value in values.items():
            setattr(self, "rebar_" + ("fill" if key=="material" else key), value)
        if self.rebar_fill in {'2','5'} and 'cover' in values:self.boolean_wood_deepness=values['cover']
    live.touch_prop(self, context)

def rebar_of(source):
    uid = source.get("project_damage_uid")
    return next((o for o in bpy.data.objects if uid is not None
                 and o.type == 'MESH' and o.get(REBAR_STAMP) == uid), None)

def _rebar_material(kind='0'):
    if kind=='5':kind='2'
    from .material_defaults import get
    if kind=='2':return get('wood_planks')
    if kind in {'0','1'}:return get('rebar' if kind=='0' else 'brick')
    if kind!='0':
        name='ProjectDamage_BooleanBrick' if kind=='1' else 'ProjectDamage_BooleanWood'
        mat=bpy.data.materials.get(name)
        if mat is None:
            mat=bpy.data.materials.new(name);enable_nodes(mat)
            mat.diffuse_color=(.45,.17,.07,1) if kind=='1' else (.28,.13,.045,1)
            bsdf=mat.node_tree.nodes.get('Principled BSDF')
            bsdf.inputs['Base Color'].default_value=mat.diffuse_color
            bsdf.inputs['Roughness'].default_value=.85
        return mat
    mat = bpy.data.materials.get("ProjectDamage_Rebar")
    if mat is not None:
        return mat
    mat = bpy.data.materials.new("ProjectDamage_Rebar")
    mat.diffuse_color = (0.33, 0.17, 0.09, 1.0)
    enable_nodes(mat)
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    if bsdf is None:
        return mat
    bsdf.inputs["Metallic"].default_value = 0.55
    bsdf.inputs["Roughness"].default_value = 0.6
    attr = nt.nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "rebar_exposed"
    attr.location = (-600, 200)
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = 'RGBA'
    mix.location = (-300, 200)
    mix.inputs[6].default_value = (0.16, 0.15, 0.14, 1.0)                              
    mix.inputs[7].default_value = (0.36, 0.17, 0.07, 1.0)                       
    nt.links.new(attr.outputs["Fac"], mix.inputs[0])
    nt.links.new(mix.outputs[2], bsdf.inputs["Base Color"])
    return mat

def _rebar_mesh(name, points, polys, bar, exposure, kind='0', part=None):
    """Fast mesh build for the (N,3)/(M,4) arrays; d == c marks a triangle."""
    tri = polys[:, 2] == polys[:, 3]
    sizes = np.where(tri, 3, 4).astype(np.int32)
    mask = np.ones(polys.shape, dtype=bool)
    mask[tri, 3] = False
    loops = polys[mask].astype(np.int32)
    starts = np.zeros(len(polys), dtype=np.int32)
    starts[1:] = np.cumsum(sizes)[:-1]
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(points))
    mesh.vertices.foreach_set("co", points.astype(np.float32).ravel())
    mesh.loops.add(len(loops))
    mesh.loops.foreach_set("vertex_index", loops)
    mesh.polygons.add(len(polys))
    mesh.polygons.foreach_set("loop_start", starts)
    try:
        mesh.polygons.foreach_set("loop_total", sizes)                  
    except (AttributeError, RuntimeError, TypeError):
        pass
    mesh.update(calc_edges=True)
    mesh.validate(clean_customdata=False)
    mesh.polygons.foreach_set("use_smooth", np.full(len(polys),kind=='0', dtype=bool))
    a = mesh.attributes.new("rebar_id", 'INT', 'FACE')
    a.data.foreach_set("value", bar.astype(np.int32))
    e = mesh.attributes.new("rebar_exposed", 'FLOAT', 'POINT')
    e.data.foreach_set("value", exposure.astype(np.float32))
    if kind in {'3','4'}:
        return mesh
    mesh.materials.append(_rebar_material(kind))
    if kind=='1' and part is not None and len(part)==len(polys):
        values=np.asarray(part,dtype=np.int32)
        attr=mesh.attributes.new('rebar_part','INT','FACE')
        attr.data.foreach_set('value',values)
        if np.any(values==1):
            mesh.materials.append(_mortar_material())
            mesh.polygons.foreach_set('material_index',(values==1).astype(np.int32))
    return mesh

def _mortar_material():
    from .material_defaults import get
    return get('mortar')

def _place_rebar(context, source, out, factor):
    """Own one optional rebar object per source; refresh or remove it safely.
    Call next to _place_deposits in rebuild_object:
        rebar = _place_rebar(context, obj, out, factor)
        if rebar is not None:
            reports.append(({'INFO'}, "ProjectDamage: %d rebar pieces" % out["rebar_bars"]))"""
    uid = source.get("project_damage_uid")
    existing = rebar_of(source)
    polys = out.get("rebar_polys")
    frames = out.get('rebar_brick_frame')
    if (source.project_damage.rebar_fill == '1' and source.project_damage.rebar_brick_3d
            and frames is not None and len(frames)):
        from . import brick3d
        placed = dict(out)
        placed['rebar_brick_frame'] = np.asarray(frames).copy() / factor
        temporary = brick3d.place_bricks(placed, source,
            model_collection=source.project_damage.rebar_brick_collection,
            collection=output_collections.destination(context,source))
        if existing is None:
            existing = temporary
        else:
            old_mesh = existing.data
            existing.data = temporary.data
            bpy.data.objects.remove(temporary, do_unlink=True)
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        existing[REBAR_STAMP] = uid
        existing.project_damage_og_object = source
        output_collections.link_object(context,source,existing)
        existing.matrix_world = source.matrix_world
        from .material_defaults import get
        material = source.project_damage.rebar_brick_material or get('brick')
        if material is not None:
            for i in range(len(existing.data.materials)):
                existing.data.materials[i] = material
            if not existing.data.materials:
                existing.data.materials.append(material)
        return existing
    if polys is None or len(polys) == 0:
        if existing is not None:
            old_mesh = existing.data
            bpy.data.objects.remove(existing, do_unlink=True)
            if old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
        return None
    kind='0' if source.project_damage.hang_enable and not source.project_damage.rebar_enable else source.project_damage.rebar_fill
    if kind=='5':kind='2'
    name = source.name + {'0':'_project_damage_rebar','1':'_project_damage_boolean_bricks','2':'_project_damage_boolean_planks','3':'_project_damage_boolean_framing','4':'_project_damage_boolean_chips'}[kind]
    mesh = _rebar_mesh(name, out["rebar_points"] / factor, polys, out["rebar_bar"], out["rebar_exposure"],kind,out.get('rebar_part'))
    if kind=='3':
        _framing.finish_mesh(mesh, source, out.get('rebar_part'), out.get('rebar_grain'))
    if kind=='4':
        _chips.finish_mesh(mesh, source, out.get('rebar_grain'))
    if existing is None:
        existing = bpy.data.objects.new(name, mesh)
    else:
        old_mesh = existing.data
        existing.data = mesh
        if old_mesh.users == 0:
            bpy.data.meshes.remove(old_mesh)
    existing.name=name
    existing[REBAR_STAMP] = uid
    existing.project_damage_og_object = source
    output_collections.link_object(context,source,existing)
    material = getattr(source.project_damage,{'0':'rebar_material','1':'rebar_brick_material','2':'rebar_plank_material','3':'rebar_framing_material','4':'rebar_chips_material'}[kind])
    if material is not None and kind not in {'3','4'}:
        mesh.materials[0]=material
    if source.project_damage.hang_enable and kind=='0':
        parts=np.asarray(out.get('rebar_part',()),dtype=np.int32)
        if len(parts)==len(mesh.polygons):
            a=mesh.attributes.get('rebar_part') or mesh.attributes.new('rebar_part','INT','FACE')
            a.data.foreach_set('value',parts)
            cables=parts==2
            if cables.any():
                cable=source.project_damage.hang_cable_material
                if cable is None:
                    cable=bpy.data.materials.get('ProjectDamage_Cable_Rubber')
                    if cable is None:
                        cable=bpy.data.materials.new('ProjectDamage_Cable_Rubber');enable_nodes(cable)
                        cable.diffuse_color=(.018,.022,.026,1.)
                        bsdf=cable.node_tree.nodes.get('Principled BSDF')
                        bsdf.inputs['Base Color'].default_value=cable.diffuse_color
                        bsdf.inputs['Roughness'].default_value=.65
                mesh.materials.append(cable)
                indices=np.zeros(len(mesh.polygons),dtype=np.int32);indices[cables]=len(mesh.materials)-1
                mesh.polygons.foreach_set('material_index',indices)
    if kind=='1' and len(mesh.materials)>1 and source.project_damage.rebar_mortar_material is not None:
        mesh.materials[1]=source.project_damage.rebar_mortar_material
    existing.matrix_world = source.matrix_world
    return existing

REBAR_MODE_ITEMS = (
    ('0', "Around Damage", "Bars are laid out where the breakers cut; exposed bars bow, snap, bend and get trimmed"),
    ('1', "Built In", "One fixed skeleton is built inside the whole object; the damage only reveals it "
                      "(moving a breaker shows other parts of the same bars)"),
)

REBAR_FIT_ITEMS = (
    ('0', "Auto", "Object Box for box-like objects (walls, slabs, columns), Follow Shape for everything else"),
    ('1', "Object Box", "The layout fills the object's bounding box (the original behaviour)"),
    ('2', "Follow Shape", "Statues, sculpts, organic parts: a grid under the damaged surface, "
                          "or a cage of bars and hoops that follows the body"),
)

def fill_strokes(context,source,core_params,factor):
    from . import paint
    p=source.project_damage
    if not core_params.enable or (p.boolean_central_rebar and p.rebar_fill == '0'):return
    paths=paint.read_paths(context,source,p.rebar_drawing,min_points=2)
    if paths:
        points=np.ascontiguousarray(np.concatenate(paths),dtype=np.float64)*factor
        core_params.set_strokes(points.ravel().tolist(),[len(path) for path in paths])

def draw_fill_type(layout, p):
    row = layout.row(align=True)
    for identifier, name, *_ in REBAR_FILL_ITEMS:
        if identifier not in {_chips.FILL_ITEM[0], '5'}:
            row.prop_enum(p, 'rebar_fill', identifier, text=name)


def draw_fill(layout,p):
    from . import paint
    if p.rebar_fill=='3':
        _framing.draw(layout,p)
        return
    if p.rebar_fill=='4':
        _chips.draw(layout,p)
        return
    brick=p.rebar_fill=='1'
    layout.prop(p,'rebar_brick_material' if brick else 'rebar_plank_material')
    for key in ('mode','fit','layout','seed','core','show_all'):
        layout.prop(p,'rebar_'+key)
    if p.rebar_mode=='1':layout.prop(p,'rebar_deform')
    col=layout.column(heading='Brick Layer' if brick else 'Wood Planks')
    for key in (('brick_length','brick_height','brick_depth','mortar','missing','brick_broken','chamfer','brick_jitter') if brick else ('plank_dir','plank_width','plank_thickness','plank_gap','missing','splinter','broken','bow','trim')):
        col.prop(p,'rebar_'+key)
    if brick:
        col.prop(p,'rebar_brick_3d',text='3D Bricks (Experimental)')
        if p.rebar_brick_3d:
            col.prop(p,'rebar_brick_collection',text='Brick Collection')
        joint=col.column();joint.active=not p.rebar_brick_3d
        joint.prop(p,'rebar_mortar_joints')
        row=joint.column();row.active=p.rebar_mortar_joints
        row.prop(p,'rebar_mortar_recess');row.prop(p,'rebar_mortar_material')
    col=layout.column(heading='Layout / Exposure')
    from . import break_surface
    col.prop(p,'rebar_scale')
    break_surface.draw_rebar_axes(col,p)
    for key in ('side','angle','cover','embed','drop_loose','detail'):
        col.prop(p,'rebar_'+key)
    ink=layout.box();ink.label(text='Hand-Drawn Courses' if brick else 'Hand-Drawn Boards',icon='GREASEPENCIL')
    paint.drawing_row(ink,p,'REBAR')
    if not brick:ink.prop(p,'rebar_stroke_radius',text='Drawn Board Width')
    ink.prop(p,'rebar_stroke_embed')

for _preset in REBAR_PRESETS.values():_preset["material"]="0"
REBAR_PRESETS.update({'11': {'material': '1', 'mode': '0', 'fit': '0', 'reach': 0.0, 'layout': '0', 'cover': 2.0, 'brick_length': 25.0, 'brick_height': 6.5, 'brick_depth': 12.0, 'mortar': 1.0, 'missing': 0.3, 'brick_broken': 0.3, 'scale': 1.0}, '12': {'material': '1', 'mode': '0', 'fit': '0', 'reach': 0.0, 'layout': '3', 'cover': 2.0, 'brick_length': 22.0, 'brick_height': 6.5, 'brick_depth': 11.0, 'mortar': 1.0, 'missing': 0.3, 'brick_broken': 0.3, 'scale': 1.0}, '13': {'material': '1', 'mode': '1', 'fit': '2', 'reach': 0.0, 'layout': '2', 'cover': 1.5, 'brick_length': 25.0, 'brick_height': 6.5, 'brick_depth': 12.0, 'mortar': 1.0, 'missing': 0.3, 'brick_broken': 0.35, 'scale': 0.5}, '14': {'material': '2', 'mode': '0', 'fit': '0', 'reach': 0.0, 'layout': '0', 'cover': 1.5, 'plank_width': 12.0, 'plank_thickness': 2.5, 'plank_gap': 0.6, 'plank_dir': '0', 'missing': 0.3, 'broken': 0.7, 'splinter': 0.6, 'trim': 0.35, 'scale': 1.0}, '15': {'material': '2', 'mode': '1', 'fit': '2', 'reach': 25.0, 'layout': '2', 'cover': 1.0, 'plank_width': 8.0, 'plank_thickness': 2.0, 'plank_gap': 0.4, 'plank_dir': '1', 'missing': 0.3, 'broken': 0.7, 'splinter': 0.7, 'trim': 0.35, 'scale': 1.0}})
REBAR_PRESET_ITEMS += (('11', 'Brick Wall', 'One brick layer behind a plastered wall'), ('12', 'Brick Column', 'Brick courses around a round column'), ('13', 'Brick Core', 'Statues: half-size brick courses that follow the body'), ('14', 'Wood Planks', 'Horizontal boards behind the surface'), ('15', 'Wood Staves', 'Statues: vertical boards that follow the body, revealed by the damage'))

REBAR_PRESETS.update(_framing.PRESETS)
REBAR_PRESET_ITEMS += _framing.PRESET_ITEMS
