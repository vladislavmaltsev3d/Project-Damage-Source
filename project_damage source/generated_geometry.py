# внутреннее заполнение можно убрать, не трогая сами повреждения
FAMILIES = ('boolean', 'concrete', 'hang', 'lath', 'brick', 'pipe', 'cable', 'fissure', 'ground', 'surface_rebar')
FIELDS = tuple(key + '_generated_enable' for key in FAMILIES)


def enabled(props, family):
    return bool(getattr(props, family + '_generated_enable', True))


def add_properties(cls):
    import bpy
    from . import live
    for name in FIELDS:
        cls.__annotations__[name] = bpy.props.BoolProperty(
            name='Generated Geometry', default=True, update=live.touch_prop,
            description='Enable: generate the internal elements with their saved settings. Disable: keep the damage openings without the internal elements. Import Manual Generators for this effect to place them yourself')


def draw(layout, props, family):
    row = layout.row(align=True)
    row.scale_y = 1.3
    on = enabled(props, family)
    row.prop(props, family + '_generated_enable', toggle=True,
             text='Enable / Disable Generated Geometry', icon='CHECKBOX_HLT' if on else 'CHECKBOX_DEHLT')
    if not on:
        layout.label(text='Import Manual Generators for this effect', icon='INFO')
    return on


def fill(params, props):
    # значения для разреза сохраняем: глубина часто зависит от толщины заполнения
    for family, block_name in (('boolean', 'break_rebar'), ('concrete', 'concrete'),
                               ('lath', 'lath'), ('brick', 'brick'), ('fissure', 'fissure'), ('ground', 'ground')):
        block = getattr(params, block_name)
        if hasattr(block, 'generated_geometry'):
            block.generated_geometry = enabled(props, family)
        elif not enabled(props, family):
            raise RuntimeError('Restart Blender to use the updated generated geometry switch')
    if not enabled(props, 'hang'):
        params.hang.bars_enable = params.hang.cables_enable = False
