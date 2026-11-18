# служебные правки интерфейса, сохранение настроек и диагностика
# SPDX-License-Identifier: GPL-3.0-or-later
"""User defaults and sidebar presentation overrides; no executable presets."""
import json
from functools import lru_cache
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
import bpy
from bpy_extras.io_utils import ExportHelper

_cache = {}
_enum_items = []
_layout_revision = 0


@lru_cache(maxsize=1)
def storage_path():
    return Path(bpy.utils.user_resource('CONFIG')) / 'project_damage' / 'dev_tools.json'


@lru_cache(maxsize=1)
def _storage_key():
    return str(storage_path())


def layout_revision():
    return (_layout_revision, id(data()['layout']))


def data():
    path = _storage_key()
    if path not in _cache:
        try:
            value = json.loads(Path(path).read_text())
            if not isinstance(value, dict) or value.get('schema') != 1:
                value = {}
        except (OSError, ValueError):
            value = {}
        _cache[path] = value
    result = _cache[path]
    result.setdefault('schema', 1)
    if not isinstance(result.get('defaults'), dict): result['defaults'] = {}
    if not isinstance(result.get('layout'), dict): result['layout'] = {}
    return result


def save(value):
    global _layout_revision
    path = storage_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False)+'\n')
    temporary.replace(path)
    _cache[str(path)] = value
    _layout_revision += 1


def selected(context):
    from . import sidebar
    src = sidebar.source(context)
    if src is None:
        return None, None
    rows = src.project_damage_effects
    index = src.project_damage_effect_index
    key = rows[index].key if 0 <= index < len(rows) else None
    return src, key if key in sidebar.CATALOG else None


def effect_name(key, fallback):
    return data()['layout'].get('names', {}).get(key) or fallback


def tiles(rows):
    config = data()['layout']
    order = config.get('order', [])
    hidden = config.get('hidden', [])
    return sorted((row for row in rows if row[0] not in hidden),
                  key=lambda row: order.index(row[0]) if row[0] in order else len(order)+rows.index(row))


class _Recorder:
    """Read only the active mode's controls, without evaluating operators."""
    def __init__(self, props):
        self.owner = props
        self.names = []
    def row(self, *args, **kwargs): return self
    column = box = split = grid_flow = column_flow = row
    def panel(self, *args, **kwargs): return self, self
    def prop(self, owner, name, **kwargs):
        if owner == self.owner and name not in self.names:
            self.names.append(name)
    def prop_search(self, owner, name, *args, **kwargs): self.prop(owner, name)
    def prop_enum(self, owner, name, value, **kwargs): self.prop(owner, name)
    def operator(self, *args, **kwargs): return SimpleNamespace()
    def __getattr__(self, name): return lambda *args, **kwargs: None


def visible_fields(context, key):
    from . import sidebar
    src = sidebar.source(context)
    if src is None or key not in sidebar.CATALOG:
        return []
    recorder = _Recorder(src.project_damage)
                                                                         
    sidebar._draw_effect_original(recorder, context, key, raw=True)
    return recorder.names


def default_fields(context, key):
    from . import sidebar
    p = sidebar.source(context).project_damage
    prefix = {'edge': ('chip_', 'chips_'), 'surface': ('surf_',),
              'corner': ('corner_',), 'fracture': ('fract_',), 'fracture_through': ('fract_',),
              'hit_through': ('hit_',), 'glass_through': ('glass_',),
              'buildup': ('build_', 'deposit_'), 'boolean': ('break_', 'rebar_')}.get(key, (key+'_',))
    names = set(visible_fields(context, key))
    if key in sidebar.TYPE_BY_ASSET:prefix=('fract_',)
    names.update(prop.identifier for prop in p.bl_rna.properties if prop.identifier.startswith(prefix))
    global_names = {'all_off','seed','global_scale','global_detail','draft_enable','draft_quality','topology',
                    'preserve_data','keep_uvs','keep_selections','live_update','defer_ms'}
    return [name for name in sorted(names) if name not in sidebar.ENABLE_PROPS
            and name not in global_names and p.bl_rna.properties[name].type in {'BOOLEAN', 'INT', 'FLOAT', 'ENUM'}]


def apply_new_defaults(src):
    """Only once per source, before initialization; never overwrite explicit edits."""
    if src.get('project_damage_dev_defaults_v1'):
        return
    if not src.get('project_damage_uid'):
        from . import sidebar, live
        p = src.project_damage
        applying = live._APPLYING
        live._APPLYING = True
        try:
            for key, values in data()['defaults'].items():
                if key not in sidebar.CATALOG or not isinstance(values, dict):
                    continue
                for name, value in values.items():
                    prop = p.bl_rna.properties.get(name)
                    if (prop is None or prop.type not in {'BOOLEAN','INT','FLOAT','ENUM'}
                            or name in sidebar.ENABLE_PROPS or name == 'all_off' or p.is_property_set(name)):
                        continue
                    try:
                        setattr(p, name, value)
                    except (ValueError, TypeError, OverflowError):
                        continue
        finally:
            live._APPLYING = applying
    src['project_damage_dev_defaults_v1'] = True


def field_config(key):
    return data()['layout'].get('fields', {}).get(key, {})


def apply_effect_defaults(src, key):
    """Also cover adding a previously unused effect to an existing setup."""
    from . import sidebar, live
    if key not in sidebar.CATALOG:
        return
    from . import quality_defaults
    quality_defaults.apply(src,key)
    values = data()['defaults'].get(key, {})
    if not isinstance(values, dict):
        return
    if key == 'fracture_glass':
                                                                                
        values = {
            'fract_rim_jag': 40.0,
            'fract_flare': 35.0,
            'fract_l1_paint_radius': 3.0,
            'fract_l1_width': 2.0,
            'fract_l1_depth': 6.0,
            'fract_l2_amount': 19,
            'fract_l2_width': 1.0,
            'fract_l2_depth': 3.5,
            'fract_l2_paint_length': 60.0,
            'fract_l2_paint_distortion': 0.0,
            'fract_l2_branches': 0.0,
            'fract_l3_amount': 0,
            **values,
        }
    p = src.project_damage
    previous = live._APPLYING
    live._APPLYING = True
    try:
        for name, value in values.items():
            prop = p.bl_rna.properties.get(name)
            if (prop is None or prop.type not in {'BOOLEAN','INT','FLOAT','ENUM'}
                    or name in sidebar.ENABLE_PROPS or name == 'all_off' or p.is_property_set(name)):
                continue
            try:
                setattr(p,name,value)
            except (ValueError,TypeError,OverflowError):
                continue
    finally:
        live._APPLYING = previous


def draw_property(layout, owner, name, key, **kwargs):
    config = field_config(key)
    if name in config.get('hidden', []) or name in config.get('pinned', []):
        return
    label = config.get('labels', {}).get(name)
    if label:
        kwargs['text'] = label
    from .material_controls import is_material_property
    if is_material_property(owner, name):
        from .material_library import draw_property
        draw_property(layout, owner, name, **kwargs)
    else:
        from . import quality_ui
        quality_ui.draw(layout, owner, name, **kwargs)


def draw_pinned(layout, context, key):
    config = field_config(key)
    fields = visible_fields(context, key) if config.get('pinned') else []
    names = [name for name in config.get('pinned', []) if name in fields and name not in config.get('hidden', [])]
    if not names:
        return
    src, _ = selected(context)
    box = layout.box()
    box.label(text='Quick Controls')
    box.enabled = not src.project_damage.all_off
    for name in names:
        label = config.get('labels', {}).get(name)
        box.prop(src.project_damage, name, **({'text': label} if label else {}))


def field_items(self, context):
    global _enum_items
    src, key = selected(context)
    if key is None:
        return [('NONE', 'Select an effect', '')]
    p = src.project_damage
    _enum_items = [(name, p.bl_rna.properties[name].name, p.bl_rna.properties[name].description)
                   for name in visible_fields(context, key)]
    return _enum_items or [('NONE', 'No controls', '')]


class ProjectDamageDevState(bpy.types.PropertyGroup):
    edit_layout: bpy.props.BoolProperty(name='Edit UI Layout', default=False,
        description='Rename and hide controls, pin/reorder quick controls and reorder/hide Add tiles; changes are saved on this computer')
    effect_label: bpy.props.StringProperty(name='Effect Name', description='New display name for the selected effect; empty restores its original name')
    field: bpy.props.EnumProperty(name='Control', items=field_items)
    field_label: bpy.props.StringProperty(name='Control Name', description='New display label for the chosen parameter; empty restores its original label')


# действие «Edit ProjectDamage UI»
class PROJECT_DAMAGE_OT_dev_action(bpy.types.Operator):
    bl_idname = 'project_damage.dev_action'
    bl_label = 'Edit ProjectDamage UI'
    bl_description = 'Save defaults or edit display names, visibility and ordering; does not change the evaluation order'
    action: bpy.props.StringProperty()

    @classmethod
    def poll(cls, context):
        return selected(context)[0] is not None

    def execute(self, context):
        from . import sidebar
        src, key = selected(context)
        if self.action == 'SAVE_DEFAULTS':
            return bpy.ops.project_damage.export_defaults_txt('INVOKE_DEFAULT')
        if key is None and self.action not in {'RESET_LAYOUT','SHOW_ALL'}:
            self.report({'WARNING'}, 'Select an effect in the hierarchy first')
            return {'CANCELLED'}
        state = context.window_manager.project_damage_dev
                                                                                     
        value = json.loads(json.dumps(data()))
        config = value['layout']
        if self.action == 'SAVE_DEFAULTS':
            values = {}
            for name in default_fields(context, key):
                prop = src.project_damage.bl_rna.properties[name]
                current = getattr(src.project_damage, name)
                values[name] = list(current) if getattr(prop, 'is_array', False) else current
            value['defaults'][key] = values
        elif self.action == 'RESET_DEFAULTS':
            value['defaults'].pop(key, None)
        elif self.action == 'RESET_LAYOUT':
            value['layout'] = {}
        elif self.action == 'RENAME_EFFECT':
            config.setdefault('names', {})[key] = state.effect_label.strip()
        elif self.action == 'SHOW_ALL':
            config['hidden'] = []
        elif self.action == 'TOGGLE_TILE':
            hidden = config.setdefault('hidden', [])
            hidden.remove(key) if key in hidden else hidden.append(key)
        elif self.action in {'TILE_UP','TILE_DOWN'}:
            order = config.setdefault('order', [r[0] for r in sidebar.EFFECTS])
            order[:] = [k for k in order if k in sidebar.CATALOG]
            order.extend(k for k in sidebar.CATALOG if k not in order)
            i = order.index(key)
            j = max(0, min(len(order)-1, i+(-1 if self.action=='TILE_UP' else 1)))
            order[i], order[j] = order[j], order[i]
        else:
            name = state.field
            if name not in visible_fields(context, key):
                self.report({'WARNING'}, 'Choose a control for the current effect and mode')
                return {'CANCELLED'}
            fields = config.setdefault('fields', {}).setdefault(key, {})
            if self.action == 'RENAME_FIELD':
                fields.setdefault('labels', {})[name] = state.field_label.strip()
            elif self.action == 'TOGGLE_FIELD':
                hidden = fields.setdefault('hidden', [])
                hidden.remove(name) if name in hidden else hidden.append(name)
            elif self.action in {'PIN','UNPIN','FIELD_UP','FIELD_DOWN'}:
                order = fields.setdefault('pinned', [])
                if self.action == 'UNPIN':
                    if name in order: order.remove(name)
                else:
                    if name not in order: order.append(name)
                    i = order.index(name)
                    if self.action in {'FIELD_UP','FIELD_DOWN'}:
                        j = max(0,min(len(order)-1,i+(-1 if self.action=='FIELD_UP' else 1)))
                        order[i],order[j] = order[j],order[i]
            else:
                return {'CANCELLED'}
        try:
            save(value)
        except (OSError, ValueError) as exc:
            self.report({'ERROR'}, 'Could not save DEV TOOLS: '+str(exc))
            return {'CANCELLED'}
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'VIEW_3D': area.tag_redraw()
        self.report({'INFO'}, ('Saved defaults for '+effect_name(key,sidebar.CATALOG[key][1])
                              if self.action=='SAVE_DEFAULTS' else 'ProjectDamage preferences saved'))
        return {'FINISHED'}


def button(layout, text, action, icon='NONE'):
    op = layout.operator('project_damage.dev_action', text=text, icon=icon)
    op.action = action


def export_snapshot(context):
    """Portable, declarative AI handoff; no mesh data or executable scripts."""
    from . import core_loader, sidebar
    src, selected_key = selected(context)
    preferences = json.loads(json.dumps(data()))
    ordered = preferences['layout'].get('order', [])
    rows = sorted(sidebar.EFFECTS,
                  key=lambda row: ordered.index(row[0]) if row[0] in ordered else len(ordered)+sidebar.EFFECTS.index(row))
    effects = []
    for key, label, enable_prop, _ in rows:
        config = field_config(key)
        names = visible_fields(context,key) if src is not None else []
        controls = []
        for name in names:
            prop = src.project_damage.bl_rna.properties[name]
            controls.append({'id':name, 'original_label':prop.name,
                             'label':config.get('labels',{}).get(name) or prop.name,
                             'hidden':name in config.get('hidden',[]),
                             'pinned':name in config.get('pinned',[]),
                             'type':prop.type, 'description':prop.description})
        effects.append({'id':key,'original_name':label,
                        'name':effect_name(key,label),
                        'tile_label':effect_name(key,sidebar.TILE_LABELS[key]),
                        'tile_hidden':key in preferences['layout'].get('hidden',[]),
                        'enable_property':enable_prop,
                        'quick_controls_order':list(config.get('pinned',[])),
                        'current_mode_controls':controls})
    values = {}
    if src is not None:
        for prop in src.project_damage.bl_rna.properties:
            name = prop.identifier
            if prop.type not in {'BOOLEAN','INT','FLOAT','ENUM','STRING'} or name.startswith(('heat_','rust_','bevel_','wear_')):
                continue
            value = getattr(src.project_damage,name)
            values[name] = list(value) if getattr(prop,'is_array',False) or isinstance(value,set) else value
    return {'format':'project_damage_ui_handoff', 'schema':1,
            'addon_version':core_loader.addon_version(),
            'blender_version':bpy.app.version_string,
            'exported_at_utc':datetime.now(timezone.utc).isoformat(),
            'preferences':preferences,
            'resolved_ui':{'panels':['Add','Presets','Tools','Settings'],
                           'effects':effects},
            'source_snapshot':{'object':src.name if src is not None else None,
                               'selected_effect':selected_key,'values':values},
            'scope':{'preferences':'All saved DEV TOOLS defaults and layout overrides, across all effects and modes.',
                     'resolved_ui':'Control discovery reflects the selected source and its current modes; all-mode overrides remain in preferences.',
                     'source_snapshot':'Reference values for this source, NOT defaults unless explicitly saved in preferences.defaults.',
                     'excluded':['mesh geometry','material datablocks','object/collection/drawing links'],
                     'presentation_only':True}}


# действие «Save All Settings for AI»
class PROJECT_DAMAGE_OT_export_dev_settings(bpy.types.Operator, ExportHelper):
    bl_idname = 'project_damage.export_dev_settings'
    bl_label = 'Save All Settings for AI'
    bl_description = 'Export all saved UI names, visibility, ordering and effect defaults to one JSON file for rebuilding the panel with an AI agent'
    filename_ext = '.json'
    filter_glob: bpy.props.StringProperty(default='*.json', options={'HIDDEN'})

    def invoke(self, context, event):
        if not self.filepath:
            self.filepath = 'project_damage_ui_settings.json'
        return ExportHelper.invoke(self,context,event)

    def execute(self, context):
        if not self.filepath:
            self.report({'ERROR'},'Choose a destination JSON file')
            return {'CANCELLED'}
        path = Path(bpy.path.abspath(bpy.path.ensure_ext(self.filepath,'.json')))
        try:
            text = json.dumps(export_snapshot(context),indent=2,ensure_ascii=False,allow_nan=False)+'\n'
            path.write_text(text)
        except (OSError,ValueError,TypeError) as exc:
            self.report({'ERROR'},'Could not export settings: '+str(exc))
            return {'CANCELLED'}
        self.report({'INFO'},'Saved all ProjectDamage settings for AI: '+str(path))
        return {'FINISHED'}


def defaults_txt_snapshot(context):
    """A reference snapshot only; never applies defaults or changes the object."""
    from . import sidebar
    src,key=selected(context)
    if src is None or key is None:
        raise ValueError('Select an effect in Settings first')
    def values(group):
        result={}
        for prop in group.bl_rna.properties:
            name=prop.identifier
            if name=='rna_type' or prop.type=='COLLECTION':continue
            value=getattr(group,name)
            if prop.type=='POINTER':
                if isinstance(value,bpy.types.ID):result[name]={'datablock':value.name,'type':type(value).__name__}
                continue
            if prop.type not in {'BOOLEAN','INT','FLOAT','ENUM','STRING'}:continue
            result[name]=list(value) if getattr(prop,'is_array',False) or isinstance(value,set) else value
        return result
                                                                           
                                                           
    groups={'project_damage':values(src.project_damage)}
    for name in ('project_damage_physics','project_damage_ivy','project_damage_roots','project_damage_wallhole'):
        group=getattr(src,name,None)
        if group is not None:groups[name]=values(group)
    if hasattr(context.scene,'project_damage_quick_physics'):
        groups['scene.project_damage_quick_physics']=values(context.scene.project_damage_quick_physics)
    return {'purpose':'Reference values for editing script defaults; not an executable preset',
            'object':src.name,'selected_effect':key,'effect_name':sidebar.CATALOG[key][1],
            'blender':bpy.app.version_string,'values':groups}


# действие «Save Values to TXT»
class PROJECT_DAMAGE_OT_export_defaults_txt(bpy.types.Operator, ExportHelper):
    bl_idname='project_damage.export_defaults_txt'
    bl_label='Save Values to TXT'
    bl_description='Choose a TXT file for the current values; send it to your agent to edit script defaults'
    filename_ext='.txt'
    filter_glob:bpy.props.StringProperty(default='*.txt',options={'HIDDEN'})

    @classmethod
    def poll(cls,context):return selected(context)[1] is not None

    def invoke(self,context,event):
        if not self.filepath:self.filepath='project_damage_values.txt'
        return ExportHelper.invoke(self,context,event)

    def execute(self,context):
        try:
            snapshot=defaults_txt_snapshot(context)
            if not self.filepath:raise ValueError('Choose a destination TXT file')
            path=Path(bpy.path.abspath(bpy.path.ensure_ext(self.filepath,'.txt')))
            path.write_text('Project Damage — current values for script defaults\n\n'+json.dumps(snapshot,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')
        except (OSError,ValueError,TypeError) as exc:
            self.report({'ERROR'},'Could not export values: '+str(exc));return {'CANCELLED'}
        self.report({'INFO'},'Saved values: '+str(path));return {'FINISHED'}


# раздел «DEV TOOLS»
class OBJECT_PT_project_damage_dev_tools(bpy.types.Panel):
    bl_label = 'DEV TOOLS'
    bl_idname = 'OBJECT_PT_project_damage_dev_tools'
    bl_parent_id = 'OBJECT_PT_project_damage_settings_ordered'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "⚒ Project Damage"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return False

    def draw(self, context):
        from . import sidebar
        layout = self.layout
        src, key = selected(context)
        if key is None:
            layout.label(text='Select an effect in the hierarchy', icon='INFO')
            return
        state = context.window_manager.project_damage_dev
        layout.label(text=effect_name(key,sidebar.CATALOG[key][1]))
        row = layout.row(align=True)
        row.operator('project_damage.export_defaults_txt',text='Save Values to TXT',icon='EXPORT')
        layout.label(text='Reference file only; script defaults are not changed')
        layout.prop(state,'edit_layout')
        if not state.edit_layout:
            return
        box = layout.box();box.label(text='Effect / Add Tile')
        box.prop(state,'effect_label');button(box,'Set Name','RENAME_EFFECT')
        row=box.row(align=True)
        button(row,'Earlier','TILE_UP','TRIA_UP');button(row,'Later','TILE_DOWN','TRIA_DOWN')
        row=box.row(align=True)
        button(row,'Show Tile' if key in data()['layout'].get('hidden',[]) else 'Hide Tile','TOGGLE_TILE')
        box=layout.box();box.label(text='Parameter / Control')
        box.prop(state,'field');box.prop(state,'field_label');button(box,'Set Control Name','RENAME_FIELD')
        hidden=state.field in field_config(key).get('hidden',[])
        button(box,'Show Control' if hidden else 'Hide Control','TOGGLE_FIELD')
        row=box.row(align=True);button(row,'Pin to Quick Controls','PIN');button(row,'Unpin','UNPIN')
        row=box.row(align=True);button(row,'Earlier','FIELD_UP','TRIA_UP');button(row,'Later','FIELD_DOWN','TRIA_DOWN')
        layout.label(text='Order affects UI only, not damage processing')


CLASSES = (ProjectDamageDevState, PROJECT_DAMAGE_OT_dev_action, PROJECT_DAMAGE_OT_export_dev_settings, PROJECT_DAMAGE_OT_export_defaults_txt)


def register_state():
    bpy.types.WindowManager.project_damage_dev = bpy.props.PointerProperty(type=ProjectDamageDevState)
    bpy.types.WindowManager.project_damage_core_profiler = bpy.props.BoolProperty(
        name='Core Profiler', default=False, options={'SKIP_SAVE'},
        description='Print stage timings and process peak memory on the next core build. Diagnostic only: does not speed up geometry. Cached builds may skip stages; no environment variable required')


def unregister_state():
    if hasattr(bpy.types.WindowManager, 'project_damage_core_profiler'):
        del bpy.types.WindowManager.project_damage_core_profiler
    del bpy.types.WindowManager.project_damage_dev
    _cache.clear()
