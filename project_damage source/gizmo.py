# показываем границы плоских каттеров прямо во вьюпорте
# ProjectDamage for Blender - worn edge damage cut into mesh objects.
# Copyright (C) 2026 Cornelius Dammrich
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
"""Draws the Plane/Strip breaker footprint as a rectangle outline, since
Blender Empties have no flat-rectangle display type and empty_display_size is
uniform (and object scale is forbidden here - it would enter matrix_world and
change the cut, see props.py's _sync_display). The Empty itself becomes a
small PLAIN_AXES click handle; this module draws the actual visual.

footprint_corners() is the only geometry the draw handler consumes, kept as a
pure function because SpaceView3D draw handlers never run under --background
- it is the one thing about this module a headless test can prove.
"""
import bpy
import gpu
import mathutils
from gpu_extras.batch import batch_for_shader

from . import breaker as breaker_mod
from . import params

_HANDLE = None
_SHADER = None                                                                     

_COLOR = (1.0, 0.38, 0.06, 0.95)                                     
_COLOR_SELECTED = (1.0, 0.65, 0.18, 1.0)


def footprint_corners(length, width, thickness, mode, factor):
    """Breaker-local rectangle ring(s) for Plane/Strip modes, already divided
    by factor into Blender units (length/width/thickness are C4D
    centimeters, the same mapping every Size field on the breaker uses).
    Plane spans local X-Z at y=0; Strip is the same rectangle at
    y=+-thickness/2, mirroring CloseSheet building Strip as two Plane sheets.
    Returns (rings, tick): rings is a list of 4-corner loops (empty for
    Sphere/Custom, which draw nothing here), tick is a (start, end) segment
    along local +Y marking the cut side, or None alongside an empty rings.
    """
    if mode not in (0, 2):
        return [], None
    hl = (length * 0.5) / factor
    hw = (width * 0.5) / factor

    def ring(y):
        return [(-hl, y, -hw), (hl, y, -hw), (hl, y, hw), (-hl, y, hw)]

    if mode == 0:
        rings = [ring(0.0)]
    else:
        ht = (thickness * 0.5) / factor
        rings = [ring(-ht), ring(ht)]
    tick_len = max(length, width) * 0.1 / factor
    tick = ((0.0, 0.0, 0.0), (0.0, tick_len, 0.0))
    return rings, tick


_DRAW_ERROR_SHOWN = False


def _draw():
    global _SHADER, _DRAW_ERROR_SHOWN
    from . import gpu_ui, live
    if live._HISTORY_RESTORING:
        return
    context = bpy.context
    if context.area is None or not gpu_ui.sidebar_visible(context.area):
        return
    breakers = live._scan().breakers
    if not breakers:
        return
    factor = params.unit_factor(context)
    if _SHADER is None:
        _SHADER = gpu.shader.from_builtin('UNIFORM_COLOR')
    shader = _SHADER
    gpu.state.blend_set('ALPHA')
    gpu.state.line_width_set(2.0)
    try:
        for obj in breakers:
            if obj.type != 'EMPTY' or obj.name not in context.view_layer.objects:
                continue
            if not obj.visible_get():
                continue
            br = obj.project_damage_breaker
            mode = breaker_mod.effective_mode(obj)
            rings, tick = footprint_corners(br.length, br.width, br.thickness, mode, factor)
            if not rings:
                continue
            color = _COLOR_SELECTED if obj.select_get() else _COLOR
            shader.bind()
            shader.uniform_float("color", color)
            mw = obj.matrix_world
            for ring in rings:
                                                                             
                                                                     
                                                                             
                                                       
                closed = list(ring) + [ring[0]]
                batch = batch_for_shader(
                    shader, 'LINE_STRIP', {"pos": [mw @ mathutils.Vector(p) for p in closed]})
                batch.draw(shader)
            batch = batch_for_shader(
                shader, 'LINES', {"pos": [mw @ mathutils.Vector(p) for p in tick]})
            batch.draw(shader)
    except Exception as exc:
                                                                             
                                                             
        if not _DRAW_ERROR_SHOWN:
            _DRAW_ERROR_SHOWN = True
            print("ProjectDamage gizmo: draw failed: %r" % exc)
    finally:
        gpu.state.line_width_set(1.0)
        gpu.state.blend_set('NONE')


# подключаем свойства и действия этого модуля к Blender
def register():
    global _HANDLE
    if bpy.app.background:
        return                                                                  
    _HANDLE = bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_VIEW')


# убираем подключения модуля при выключении аддона
def unregister():
    global _HANDLE
    if _HANDLE is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_HANDLE, 'WINDOW')
        _HANDLE = None
