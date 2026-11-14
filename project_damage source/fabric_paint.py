# отдельный безопасный вход в рисование разрывов ткани
import bpy
from . import ownership

TARGETS={'FABRIC_HOLES','FABRIC_EDGES'}
STAMP='project_damage_fabric_drawing'


def is_drawing(drawing,source=None):
    if drawing is None:return False
    owner=ownership.source_of(drawing)
    if source is not None and owner!=source:return False
    if drawing.get(STAMP):return True
    source=source or owner
    if source is None:return False
    props=source.project_damage
    return drawing in (props.fabric_holes_drawing,props.fabric_edges_drawing)


def start(context,drawing):
    from . import paint
    drawing[STAMP]=True
    paint._start_drawing(context,drawing,defer_preview=True)


class PROJECT_DAMAGE_OT_fabric_paint_new(bpy.types.Operator):
    bl_idname='project_damage.fabric_paint_new'
    bl_label='New Fabric Drawing'
    bl_description='Draw fabric tears. Fabric geometry updates after leaving Draw mode; other effects keep their live drawing preview'
    bl_options={'REGISTER','UNDO'}
    target:bpy.props.EnumProperty(items=(('FABRIC_HOLES','Holes','Draw holes in the fabric'),('FABRIC_EDGES','Edges','Draw torn fabric edges')))
    @classmethod
    def poll(cls,context):
        from .paint import source_for
        return source_for(context) is not None
    def execute(self,context):
        from .paint import new_drawing
        return new_drawing(context,self.target,1)


class PROJECT_DAMAGE_OT_fabric_paint_draw(bpy.types.Operator):
    bl_idname='project_damage.fabric_paint_draw'
    bl_label='Draw Fabric Tears'
    bl_description='Edit fabric tear contours. Leave Draw mode to update Fabric when Live Update is enabled'
    bl_options={'REGISTER','UNDO'}
    target:bpy.props.EnumProperty(items=(('FABRIC_HOLES','Holes','Draw holes in the fabric'),('FABRIC_EDGES','Edges','Draw torn fabric edges')))
    def execute(self,context):
        from .paint import draw_existing
        return draw_existing(context,self.target,1)


CLASSES=(PROJECT_DAMAGE_OT_fabric_paint_new,PROJECT_DAMAGE_OT_fabric_paint_draw)
# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in CLASSES:bpy.utils.register_class(cls)
# убираем подключения модуля при выключении аддона
def unregister():
    for cls in reversed(CLASSES):bpy.utils.unregister_class(cls)
