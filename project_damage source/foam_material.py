# создаём материал пены с нужным мелким рельефом
# ProjectDamage expanding foam material - GPL-3.0-or-later; see LICENSE.
#
# Procedural polyurethane foam shader, node group "PD Expanding Foam" inside
# the material "PD_Expanding Foam" (Cycles and EEVEE).
#   skin     waxy, slightly translucent cured skin: broad lumps, fine
#            orange-peel, popped bubbles (pores) and pinholes, glossier and
#            matter patches
#   cut      knife-trimmed faces (foam_cut): open cells with thin bright walls
#            and shadowed cell bottoms, rough and velvety
#   cavity   crevices between lobes and the contact with the wall
#            (foam_cavity): darker, dust collects there (Dirt)
#   age      UV yellowing toward orange-brown, stronger on exposed lobes,
#            chalkier and rougher
#   colour   per object: the output object's "pd_foam_color" / "pd_foam_age"
#            properties (Look in the Foam settings), so one material serves
#            every foam object; foam_variation shifts hue and value a little
# Texture space is foam_coord (metres, the source's rotation without its
# scale), so pores keep their size on objects with unapplied scale.
import bpy

GROUP = 'PD Expanding Foam'
MATERIAL = 'PD_Expanding Foam'
VERSION = 2

LOOKS = {
                     
    'FRESH': ((0.855, 0.672, 0.305), 0.0),
    'CURED': ((0.80, 0.56, 0.20), 0.12),
    'AGED': ((0.50, 0.29, 0.10), 0.55),
    'WHITE': ((0.871, 0.838, 0.753), 0.0),
    'GREY': ((0.429, 0.445, 0.402), 0.0),
    'FIRE': ((0.610, 0.080, 0.080), 0.0),
}
DEFAULT_COLOR, DEFAULT_AGE = LOOKS['CURED']


class _G:
    """Small node-building helper."""

    def __init__(self, tree, x=0, y=0):
        self.t = tree; self.n = tree.nodes; self.l = tree.links

    def node(self, kind, x, y, **props):
        nd = self.n.new(kind); nd.location = (x, y)
        for k, v in props.items():
            setattr(nd, k, v)
        return nd

    def link(self, a, b):
        self.l.new(a, b)

    def math(self, op, a, b=None, x=0, y=0, clamp=False):
        nd = self.node('ShaderNodeMath', x, y, operation=op, use_clamp=clamp)
        for i, v in enumerate((a, b)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                nd.inputs[i].default_value = v
            else:
                self.link(v, nd.inputs[i])
        return nd.outputs[0]

    def mix_color(self, fac, a, b, x=0, y=0, blend='MIX'):
        nd = self.node('ShaderNodeMix', x, y, data_type='RGBA', blend_type=blend)
        for sock, v in ((nd.inputs[0], fac), (nd.inputs[6], a), (nd.inputs[7], b)):
            if isinstance(v, (int, float)):
                sock.default_value = v
            elif isinstance(v, tuple):
                sock.default_value = (*v[:3], 1.0)
            else:
                self.link(v, sock)
        return nd.outputs[2]

    def map_range(self, v, a, b, c=0.0, d=1.0, x=0, y=0, interp='LINEAR'):
        nd = self.node('ShaderNodeMapRange', x, y, interpolation_type=interp, clamp=True)
        self.link(v, nd.inputs[0])
        for i, val in ((1, a), (2, b), (3, c), (4, d)):
            if isinstance(val, (int, float)):
                nd.inputs[i].default_value = val
            else:
                self.link(val, nd.inputs[i])
        return nd.outputs[0]

    def attr(self, name, kind='GEOMETRY', x=0, y=0):
        return self.node('ShaderNodeAttribute', x, y, attribute_type=kind, attribute_name=name)


def _socket(group, name, kind, in_out='INPUT', default=None, lo=None, hi=None):
    s = group.interface.new_socket(name, in_out=in_out, socket_type=kind)
    if default is not None:
        s.default_value = default
    if lo is not None:
        s.min_value = lo
    if hi is not None:
        s.max_value = hi
    return s


def _build_group(group):
    group.nodes.clear()
    for item in list(group.interface.items_tree):
        group.interface.remove(item)
    _socket(group, 'BSDF', 'NodeSocketShader', 'OUTPUT')
    _socket(group, 'Color', 'NodeSocketColor', default=(*DEFAULT_COLOR, 1.0))
    _socket(group, 'Age', 'NodeSocketFloat', default=DEFAULT_AGE, lo=0.0, hi=1.0)
    _socket(group, 'Texture Scale', 'NodeSocketFloat', default=1.0, lo=0.05, hi=20.0)
    _socket(group, 'Pores', 'NodeSocketFloat', default=0.5, lo=0.0, hi=1.0)
    _socket(group, 'Bump', 'NodeSocketFloat', default=1.0, lo=0.0, hi=4.0)
    _socket(group, 'Roughness', 'NodeSocketFloat', default=0.5, lo=0.0, hi=1.0)
    _socket(group, 'Subsurface', 'NodeSocketFloat', default=0.25, lo=0.0, hi=1.0)
    _socket(group, 'Dirt', 'NodeSocketFloat', default=0.1, lo=0.0, hi=1.0)
    g = _G(group)
    gi = g.node('NodeGroupInput', -2200, 0)
    go = g.node('NodeGroupOutput', 1400, 0)
    I = gi.outputs

                                                                       
    coord = g.attr('foam_coord', x=-2000, y=500)
    tex = g.node('ShaderNodeTexCoord', -2000, 300)
    has = g.node('ShaderNodeVectorMath', -1800, 500, operation='LENGTH')
    g.link(coord.outputs['Vector'], has.inputs[0])
    missing = g.math('LESS_THAN', has.outputs['Value'], 1e-9, x=-1650, y=500)
    pick = g.node('ShaderNodeMix', -1500, 400, data_type='VECTOR')
    g.link(missing, pick.inputs[0])
    g.link(coord.outputs['Vector'], pick.inputs[4]); g.link(tex.outputs['Object'], pick.inputs[5])
    scl = g.node('ShaderNodeVectorMath', -1350, 400, operation='SCALE')
    g.link(pick.outputs[1], scl.inputs[0]); g.link(I['Texture Scale'], scl.inputs['Scale'])
    P = scl.outputs[0]

    var = g.attr('foam_variation', x=-2000, y=-400).outputs['Fac']
    cut = g.attr('foam_cut', x=-2000, y=-550).outputs['Fac']
    cav = g.attr('foam_cavity', x=-2000, y=-700).outputs['Fac']
    cutm = g.map_range(cut, 0.35, 0.65, x=-1800, y=-550, interp='SMOOTHSTEP')

                      
    lumps = g.node('ShaderNodeTexNoise', -1100, 700, noise_dimensions='3D')
    lumps.inputs['Scale'].default_value = 38.0; lumps.inputs['Detail'].default_value = 3.0
    lumps.inputs['Roughness'].default_value = 0.55
    g.link(P, lumps.inputs['Vector'])
    peel = g.node('ShaderNodeTexNoise', -1100, 450, noise_dimensions='3D')
    peel.inputs['Scale'].default_value = 420.0; peel.inputs['Detail'].default_value = 2.0
    g.link(P, peel.inputs['Vector'])
    pores = g.node('ShaderNodeTexVoronoi', -1100, 200, voronoi_dimensions='3D', feature='F1')
    pores.inputs['Scale'].default_value = 240.0
    g.link(P, pores.inputs['Vector'])
    pinh = g.node('ShaderNodeTexVoronoi', -1100, -50, voronoi_dimensions='3D', feature='F1')
    pinh.inputs['Scale'].default_value = 750.0
    g.link(P, pinh.inputs['Vector'])
                                                                
    sep = g.node('ShaderNodeSeparateColor', -900, 120)
    g.link(pores.outputs['Color'], sep.inputs[0])
    thr = g.math('MULTIPLY', I['Pores'], 0.6, x=-900, y=0)
    has_pore = g.math('LESS_THAN', sep.outputs[0], thr, x=-750, y=100)
                                                                                          
    psize = g.map_range(g.math('POWER', sep.outputs[1], 2.0, x=-900, y=350), 0.0, 1.0, 0.12, 0.36, x=-800, y=350)
    bowl = g.map_range(pores.outputs['Distance'], psize, g.math('MULTIPLY', psize, 0.45, x=-800, y=280), x=-650, y=250, interp='SMOOTHSTEP')
    pit = g.math('MULTIPLY', bowl, has_pore, x=-600, y=200)
    sep2 = g.node('ShaderNodeSeparateColor', -900, -150)
    g.link(pinh.outputs['Color'], sep2.inputs[0])
    thr2 = g.math('MULTIPLY', I['Pores'], 0.4, x=-900, y=-250)
    has_pin = g.math('LESS_THAN', sep2.outputs[0], thr2, x=-750, y=-150)
    pin = g.math('MULTIPLY', g.map_range(pinh.outputs['Distance'], 0.22, 0.06, x=-750, y=-300, interp='SMOOTHSTEP'),
                 has_pin, x=-600, y=-200)
    skin_h = g.math('MULTIPLY_ADD', lumps.outputs['Fac'], 0.45, x=-450, y=600)
    skin_h = g.math('ADD', skin_h, g.math('MULTIPLY', peel.outputs['Fac'], 0.05, x=-450, y=450), x=-300, y=550)
    skin_h = g.math('SUBTRACT', skin_h, g.math('MULTIPLY', pit, 0.32, x=-450, y=250), x=-200, y=450)
    skin_h = g.math('SUBTRACT', skin_h, g.math('MULTIPLY', pin, 0.12, x=-450, y=-150), x=-100, y=350)

                                                                                         
    cells = g.node('ShaderNodeTexVoronoi', -1100, -500, voronoi_dimensions='3D', feature='F1')
    cells.inputs['Scale'].default_value = 800.0; cells.inputs['Randomness'].default_value = 1.0
    g.link(P, cells.inputs['Vector'])
    csep = g.node('ShaderNodeSeparateColor', -900, -450)
    g.link(cells.outputs['Color'], csep.inputs[0])
    crad = g.map_range(csep.outputs[0], 0.0, 1.0, 0.42, 0.72, x=-750, y=-450)
    hole = g.map_range(cells.outputs['Distance'], crad, g.math('MULTIPLY', crad, 0.72, x=-750, y=-550), x=-600, y=-500, interp='SMOOTHSTEP')
    voids = g.node('ShaderNodeTexVoronoi', -1100, -750, voronoi_dimensions='3D', feature='F1')
    voids.inputs['Scale'].default_value = 230.0
    g.link(P, voids.inputs['Vector'])
    vsep = g.node('ShaderNodeSeparateColor', -900, -750)
    g.link(voids.outputs['Color'], vsep.inputs[0])
    void = g.math('MULTIPLY', g.map_range(voids.outputs['Distance'], 0.3, 0.16, x=-750, y=-800, interp='SMOOTHSTEP'),
                  g.math('LESS_THAN', vsep.outputs[1], 0.22, x=-750, y=-700), x=-600, y=-750)
    holes = g.math('MAXIMUM', hole, void, x=-450, y=-600)
    bowl_c = g.math('SUBTRACT', 1.0, holes, x=-300, y=-650)                                      
    cut_h = g.math('MULTIPLY_ADD', holes, -0.7, x=-300, y=-550)
    height = g.mix_color(cutm, skin_h, cut_h, x=0, y=200)

                 
    hsv = g.node('ShaderNodeHueSaturation', -600, -1000)
    g.link(I['Color'], hsv.inputs['Color'])
    g.link(g.map_range(var, 0.0, 1.0, 0.485, 0.515, x=-800, y=-1000), hsv.inputs['Hue'])
    g.link(g.map_range(var, 0.0, 1.0, 0.9, 1.08, x=-800, y=-1150), hsv.inputs['Value'])
    base = hsv.outputs[0]
                                         
    patch = g.node('ShaderNodeTexNoise', -1100, -1300)
    patch.inputs['Scale'].default_value = 9.0; patch.inputs['Detail'].default_value = 2.0
    g.link(P, patch.inputs['Vector'])
    expo = g.math('SUBTRACT', 1.0, g.math('MULTIPLY', cav, 0.7, x=-800, y=-1350), x=-650, y=-1350)
    age = g.math('MULTIPLY', I['Age'], expo, x=-500, y=-1300)
    age = g.math('MULTIPLY', age, g.map_range(patch.outputs['Fac'], 0.3, 0.7, 0.65, 1.25, x=-650, y=-1450), x=-400, y=-1350, clamp=True)
    aged_tint = g.mix_color(1.0, base, (0.62, 0.30, 0.07), x=-400, y=-1150, blend='MULTIPLY')
    aged_tint = g.mix_color(0.45, aged_tint, (0.40, 0.22, 0.07), x=-250, y=-1150)
    col = g.mix_color(age, base, aged_tint, x=-100, y=-1100)
                                                                                     
    shade = g.mix_color(1.0, col, (0.42, 0.26, 0.12), x=-100, y=-850, blend='MULTIPLY')                     
    col = g.mix_color(g.math('MULTIPLY', pit, 0.5, x=-250, y=-900), col, shade, x=50, y=-950)
    cutcol = g.mix_color(1.0, col, (1.18, 1.12, 1.0), x=50, y=-750, blend='MULTIPLY')
    cutcol = g.mix_color(g.map_range(bowl_c, 0.0, 1.0, 0.8, 0.0, x=-100, y=-650), cutcol, shade, x=200, y=-700)
    col = g.mix_color(cutm, col, cutcol, x=350, y=-800)
                                         
    col = g.mix_color(g.math('MULTIPLY', cav, 0.5, x=200, y=-1050), col, shade, x=500, y=-1000)
    grime = g.node('ShaderNodeTexNoise', -1100, -1600)
    grime.inputs['Scale'].default_value = 60.0; grime.inputs['Detail'].default_value = 4.0
    g.link(P, grime.inputs['Vector'])
    dirt = g.map_range(g.math('MULTIPLY_ADD', grime.outputs['Fac'], 0.6, x=-800, y=-1600), 0.55, 1.1, x=-650, y=-1600)
    dirt_amt = g.math('MULTIPLY', g.math('ADD', cav, g.math('MULTIPLY', dirt, 0.4, x=-500, y=-1650), x=-350, y=-1600),
                      I['Dirt'], x=-200, y=-1600, clamp=True)
    col = g.mix_color(dirt_amt, col, (0.16, 0.13, 0.095), x=650, y=-1050)

                  
    bsdf = g.node('ShaderNodeBsdfPrincipled', 950, 0)
    g.link(col, bsdf.inputs['Base Color'])
    gloss = g.node('ShaderNodeTexNoise', -1100, 1000)
    gloss.inputs['Scale'].default_value = 14.0; gloss.inputs['Detail'].default_value = 1.0
    g.link(P, gloss.inputs['Vector'])
    rough = g.math('ADD', I['Roughness'], g.map_range(gloss.outputs['Fac'], 0.35, 0.65, -0.12, 0.1, x=-800, y=1000), x=-500, y=1000)
    rough = g.math('ADD', rough, g.math('MULTIPLY', I['Age'], 0.25, x=-500, y=900), x=-350, y=950)
    rough = g.math('ADD', rough, g.math('MULTIPLY', pit, 0.2, x=-350, y=850), x=-200, y=950)
    rough = g.map_range(rough, 0.0, 1.0, 0.05, 1.0, x=-50, y=950)
    rough = g.math('ADD', g.math('MULTIPLY', rough, g.math('SUBTRACT', 1.0, cutm, x=100, y=850), x=250, y=900),
                   g.math('MULTIPLY', cutm, 0.92, x=250, y=800), x=400, y=900)
    g.link(rough, bsdf.inputs['Roughness'])
    bsdf.inputs['Specular IOR Level'].default_value = 0.38
    sss = g.math('MULTIPLY', I['Subsurface'], g.math('SUBTRACT', 1.0, g.math('MULTIPLY', cav, 0.6, x=500, y=500), x=650, y=500), x=800, y=500)
    g.link(sss, bsdf.inputs['Subsurface Weight'])
    bsdf.inputs['Subsurface Radius'].default_value = (1.0, 0.55, 0.22)
    bsdf.inputs['Subsurface Scale'].default_value = 0.004
    g.link(g.math('MULTIPLY', cutm, 0.35, x=650, y=350), bsdf.inputs['Sheen Weight'])
    bsdf.inputs['Sheen Roughness'].default_value = 0.6
                                                                   
    bump = g.node('ShaderNodeBump', 650, 150)
    g.link(height, bump.inputs['Height'])
    g.link(I['Bump'], bump.inputs['Strength'])
    bdist = g.math('DIVIDE', 0.0012, I['Texture Scale'], x=450, y=100)
    g.link(bdist, bump.inputs['Distance'])
    g.link(bump.outputs['Normal'], bsdf.inputs['Normal'])
    g.link(bsdf.outputs[0], go.inputs[0])


def ensure_group():
    group = bpy.data.node_groups.get(GROUP)
    if group is not None and not group.get('pd_foam_shader'):
        group=next((g for g in bpy.data.node_groups if g.get('pd_foam_shader')
                    and g.bl_idname=='ShaderNodeTree' and g.name.startswith(GROUP)),None)
    if group is not None and group.bl_idname == 'ShaderNodeTree' and group.get('pd_foam_shader', 0) >= VERSION:
        return group
    if group is None or group.bl_idname != 'ShaderNodeTree':
        group = bpy.data.node_groups.new(GROUP, 'ShaderNodeTree')
    _build_group(group)
    group['pd_foam_shader'] = VERSION
    return group


def ensure_material():
    """Append the supplied Foam library material once; explicit overrides win."""
    for mat in bpy.data.materials:
        if mat.get('project_damage_foam_library_material')==MATERIAL and not mat.get('project_damage_shader_uv'):
            return mat
    from pathlib import Path
    path=Path(__file__).parent/'blends/foam_mat.blend'
    with bpy.data.libraries.load(str(path),link=False) as (source,target):
        if MATERIAL not in source.materials:
            raise RuntimeError('Material missing in foam_mat.blend: '+MATERIAL)
        target.materials=[MATERIAL]
    material=target.materials[0]
    material['project_damage_foam_library_material']=MATERIAL
    return material


def apply_look(obj, color, age):
    """Per-object colour / age read by the shared material."""
    if obj is None:
        return
    obj['pd_foam_color'] = tuple(float(c) for c in color[:3])
    obj['pd_foam_age'] = float(age)
    obj.color = (*[float(c) for c in color[:3]], 1.0)
    obj.update_tag()
