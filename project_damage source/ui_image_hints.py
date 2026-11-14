# показываем готовые картинки и текст в окнах подсказок
# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional, local rendered comparisons. UI only: never requests geometry updates.

Add a topic in image_hint_catalog.py and JPEGs in data/hints, then call
button(row, 'topic') beside a setting, or prop(row, owner, 'property').
"""
from pathlib import Path
import textwrap
import bpy
import bpy.utils.previews
from .image_hint_catalog import HINTS, PROPERTY_HINTS

IMAGE_DIR = Path(__file__).resolve().parent / 'data' / 'hints'
_PREVIEWS = None


def enabled(context=None):
    context = context or bpy.context
    entry = context.preferences.addons.get(__package__)
    return bool(getattr(entry.preferences, 'enable_hints', True)) if entry else True


def topic(owner, name):
    identifier = getattr(getattr(owner, 'bl_rna', None), 'identifier', '')
    if identifier.startswith('ProjectDamageInnerRough'):
        return {'form': 'inner_form', 'resolution': 'inner_resolution',
                'proxy': 'physics_proxy'}.get(name)
    if identifier == 'PDPlankProps' and name == 'mode':
        return 'plank_pins'
    if identifier == 'ProjectDamageClothSettings' and name in {'source', 'curve', 'empties'}:
        return 'cloth_sources'
    if identifier == 'ProjectDamageIvySettings' and name in {'curve', 'adhesion_weight'}:
        return 'ivy_attachment'
    if identifier == 'ProjectDamageFoamSettings' and name == 'placement':
        return 'foam_placement'
    if identifier == 'ProjectDamagePhysicsSettings' and name == 'material':
        return 'fracture_type'
    return PROPERTY_HINTS.get(name)


def button(layout, key, context=None):
    if key not in HINTS or not enabled(context):
        return
                                                                       
    icon_row = layout.row(align=True)
    icon_row.scale_x = 1.0
    icon_row.scale_y = 1.0
    op = icon_row.operator('project_damage.image_hint', text='', icon='QUESTION', emboss=True)
    op.topic = key


def prop(layout, owner, name, **kwargs):
    """For native UILayout drawers that don't use sidebar._Layout."""
    if type(layout).__name__ == '_Layout':
        return layout.prop(owner, name, **kwargs)
    key = topic(owner, name)
    if key and enabled():
        row = layout.row(align=True)
        from . import quality_ui
        result = quality_ui.draw(row, owner, name, **kwargs)
        button(row, key)
        return result
    from . import quality_ui
    return quality_ui.draw(layout, owner, name, **kwargs)


def _icon(stem):
    global _PREVIEWS
    if _PREVIEWS is None:
        _PREVIEWS = bpy.utils.previews.new()
    if stem not in _PREVIEWS:
        path = IMAGE_DIR / (stem + '.jpg')
        if not path.is_file():
            return 0
        _PREVIEWS.load(stem, str(path), 'IMAGE')
    return _PREVIEWS[stem].icon_id


# собираем видимые настройки этого раздела
def draw(layout, key):
    entry = HINTS.get(key)
    if entry is None:
        layout.label(text='This hint is unavailable', icon='INFO')
        return
    title, explanation, images = entry
    heading = layout.row()
    heading.alignment = 'CENTER'
    heading.label(text=title, icon='QUESTION')
    layout.separator()
    columns = 3 if key == 'break_surface_styles' else 2
    for first in range(0, len(images), columns):
        row = layout.row(align=False)
        for stem, caption in images[first:first+columns]:
            col = row.column(align=True)
            pic = col.row()
            pic.alignment = 'CENTER'
            icon = _icon(stem)
            if icon:
                pic.template_icon(icon_value=icon, scale=8.0 if columns == 3 else 11.0)
            else:
                pic.label(text='Preview missing', icon='IMAGE_DATA')
            label = col.row()
            label.alignment = 'CENTER'
            label.label(text=caption)
        layout.separator()
    for line in textwrap.wrap(explanation, width=60):
        layout.label(text=line)
    if key == 'fracture_surface':
        layout.separator()
        op = layout.operator('project_damage.editorial_open', text='Read the Full Guide', icon='HELP')
        op.asset = 'fracture_surface'
        op.section = 'Classic / Rounded'


# действие «Visual Hint»
class PROJECT_DAMAGE_OT_image_hint(bpy.types.Operator):
    bl_idname = 'project_damage.image_hint'
    bl_label = 'Visual Hint'
    bl_description = 'See rendered examples explaining this setting'
    bl_options = {'INTERNAL'}
    topic: bpy.props.StringProperty(options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return enabled(context)

    @classmethod
    def description(cls, context, properties):
        item = HINTS.get(properties.topic)
        return ('Show visual examples: ' + item[0]) if item else cls.bl_description

    def invoke(self, context, event):
        if self.topic not in HINTS:
            return {'CANCELLED'}
        return context.window_manager.invoke_popup(self, width=840 if self.topic == 'break_surface_styles' else 580)

    def draw(self, context):
        draw(self.layout, self.topic)

    def execute(self, context):
        return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_image_hint)


# убираем подключения модуля при выключении аддона
def unregister():
    global _PREVIEWS
    if _PREVIEWS is not None:
        bpy.utils.previews.remove(_PREVIEWS)
        _PREVIEWS = None
    try:
        bpy.utils.unregister_class(PROJECT_DAMAGE_OT_image_hint)
    except RuntimeError:
        pass
