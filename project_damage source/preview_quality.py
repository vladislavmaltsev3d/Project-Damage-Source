# разделяем качество чернового просмотра и окончательного Apply
from . import quality_ui
from contextlib import contextmanager

_FINAL_SOURCES = set()


def percent(props):
    source = getattr(props, 'id_data', None)
    final = source is not None and source.as_pointer() in _FINAL_SOURCES
    if getattr(props, 'draft_enable', False) and not final:
        return float(props.draft_quality)
    return float(props.global_detail)


@contextmanager
def final_quality(source):
    key = source.as_pointer()
    nested = key in _FINAL_SOURCES
    _FINAL_SOURCES.add(key)
    try:
        yield
    finally:
        if not nested:
            _FINAL_SOURCES.discard(key)


# собираем видимые настройки этого раздела
def draw(layout, props):
    row = layout.row(align=True)
    quality_ui.draw(row, props, 'global_detail', text='Master Quality')
    toggle = row.row(align=True)
    toggle.use_property_split = False
    toggle.prop(props, 'draft_enable', text='Draft', icon='SHADING_WIRE', toggle=True)
    if props.draft_enable:
        box = layout.box()
        head = box.row()
        head.alignment = 'CENTER'
        head.label(text='DRAFT PREVIEW', icon='SHADING_WIRE')
        box.prop(props, 'draft_quality', text='Draft Quality')
        note = box.row()
        note.use_property_split = False
        note.label(text='Apply All uses Master Quality', icon='CHECKMARK')


def rebar_resolution(props, name):
    source = getattr(props, 'id_data', None)
    final = source is not None and source.as_pointer() in _FINAL_SOURCES
    if getattr(props, 'draft_enable', False) and not final:
        return 0.0
    return max(0.0, min(1.0, float(getattr(props, name, 1.0))))
