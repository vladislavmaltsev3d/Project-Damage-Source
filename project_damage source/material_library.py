# поиск, превью и назначение материалов из библиотеки ambientCG
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline built-in material picker. Libraries are appended only on selection.

Catalog: data/material_library.json. Replace PNGs under data/material previews
to supply real previews; their paths mirror the library folders/material names.
The browser freezes the target property, so pinning/selection changes are safe.
"""
from pathlib import Path
import json
import math
import time
import bpy
import bpy.utils.previews

ROOT = Path(__file__).resolve().parent
LIBRARY = ROOT / 'data/material library'
PREVIEWS = ROOT / 'data/material previews'
INDEX = ROOT / 'data/material_library.json'
PAGE_SIZE = 9
_catalog = []
_stamp = None
_icons = None
_icon_stamps = {}
_preview_queue = []
_preview_after = 0.0
_browser_available = False



CONFIG = ROOT / 'material_library_config.json'

def preferences():
    addon=bpy.context.preferences.addons.get(__package__)
    return addon.preferences if addon is not None else None


def configured_root():
    pref=preferences()
    if pref is not None and not pref.material_library_enabled:return None
    selected=pref.material_library_dir if pref is not None else ''
    if selected:return Path(bpy.path.abspath(selected)).expanduser().resolve()
    try:
        data=json.loads(CONFIG.read_text(encoding='utf-8'))
        if data.get('enabled') and data.get('folder'):return Path(data['folder']).expanduser().resolve()
    except (OSError,ValueError,TypeError):pass
    return None


def _sync_paths():
    global LIBRARY,PREVIEWS,INDEX
    root=configured_root() or ROOT/'_material_library_not_installed'
    LIBRARY=root/'material library';PREVIEWS=root/'material previews';INDEX=root/'material_library.json'
    return root


def available():
    _sync_paths()
    return INDEX.is_file() and LIBRARY.is_dir()


def invalidate():
    global _catalog,_stamp,_icons
    _preview_queue.clear()
    if bpy.app.timers.is_registered(_load_preview):bpy.app.timers.unregister(_load_preview)
    _catalog=[];_stamp=None;_icon_stamps.clear()
    if _icons is not None:
        try:bpy.utils.previews.remove(_icons)
        except (ReferenceError,KeyError):pass
        _icons=None
    for screen in bpy.data.screens:
        for area in screen.areas:area.tag_redraw()


def validate_folder(folder):
    root=Path(bpy.path.abspath(folder)).expanduser().resolve()
    if not (root/'material_library.json').is_file() and (root/'data/material_library.json').is_file():root=root/'data'
    index=root/'material_library.json'
    if not index.is_file():raise ValueError('Select the extracted ProjectDamage_CC0_Materials folder containing material_library.json')
    data=json.loads(index.read_text(encoding='utf-8'));entries=data.get('entries',[])
    if not entries:raise ValueError('The material catalog is empty')
    for entry in entries:
        if not child_path(root/'material library',entry['blend']).is_file():raise ValueError('Missing library file: '+entry['blend'])
    return root,len(entries)


def _write_config(folder,enabled):
    tmp=CONFIG.with_suffix('.tmp')
    tmp.write_text(json.dumps({'version':1,'folder':str(folder) if folder else '', 'enabled':enabled},indent=2),encoding='utf-8')
    tmp.replace(CONFIG)


# действие «Install Material Library»
class PROJECT_DAMAGE_OT_install_material_library(bpy.types.Operator):
    bl_idname='project_damage.install_material_library';bl_label='Install Material Library'
    bl_description='Select the extracted CC0 material pack folder. Files stay in the selected folder'
    directory:bpy.props.StringProperty(subtype='DIR_PATH')
    filter_folder:bpy.props.BoolProperty(default=True,options={'HIDDEN'})
    def invoke(self,context,event):
        pref=preferences()
        if not self.directory and pref is not None:self.directory=pref.material_library_dir
        context.window_manager.fileselect_add(self);return {'RUNNING_MODAL'}
    def execute(self,context):
        try:
            root,count=validate_folder(self.directory)
            pref=preferences()
            if pref is None:raise ValueError('Enable Project Damage before installing the material library')
            pref.material_library_dir=str(root);pref.material_library_enabled=True
            _write_config(root,True);invalidate()
        except (OSError,ValueError,TypeError,KeyError) as exc:
            self.report({'ERROR'},str(exc));return {'CANCELLED'}
        self.report({'INFO'},'Material library connected: %d materials'%count);return {'FINISHED'}


class PROJECT_DAMAGE_OT_remove_material_library(bpy.types.Operator):
    bl_idname='project_damage.remove_material_library';bl_label='Remove Material Library'
    bl_description='Disconnect this material library; files in the selected folder and materials already used in scenes remain'
    def execute(self,context):
        pref=preferences()
        try:_write_config(None,False)
        except OSError as exc:self.report({'ERROR'},str(exc));return {'CANCELLED'}
        if pref is not None:pref.material_library_enabled=False;pref.material_library_dir=''
        invalidate();return {'FINISHED'}


def draw_preferences(layout,pref):
    box=layout.box();box.label(text='CC0 Material Library (ambientCG / Poly Haven)',icon='MATERIAL')
    installed=available()
    if not installed:
        box.prop(pref,'material_library_dir',text='Material Library Folder')
    row=box.row(align=True);row.operator_context='INVOKE_DEFAULT'
    if installed:
        row.operator('project_damage.remove_material_library',text='Remove',icon='TRASH')
    else:
        row.operator('project_damage.install_material_library',text='Install / Connect',icon='FILE_FOLDER')
    box.label(text='Installed' if installed else 'Not installed — download and extract the separate CC0 material pack',icon='CHECKMARK' if installed else 'INFO')
    box.label(text='Remove disconnects the folder and keeps your files.')


def draw_missing_library(layout):
    layout.label(text='CC0 material library is not installed.',icon='INFO')
    layout.label(text='Download the separate Project Damage CC0 Materials pack,')
    layout.label(text='extract it, then select its folder with Install Material Library.')
    pref=preferences()
    if pref is not None and pref.material_library_download_url.startswith(('https://','http://')):
        layout.operator('wm.url_open',text='Download Material Library',icon='URL').url=pref.material_library_download_url
    row=layout.row(align=True);row.operator_context='INVOKE_DEFAULT'
    row.operator('project_damage.install_material_library',text='Install Material Library',icon='FILE_FOLDER')
    row.operator('preferences.addon_show',text='Preferences',icon='PREFERENCES').module=__package__


def catalog():
    global _catalog, _stamp
    _sync_paths()
    try:
        stamp = (str(INDEX),INDEX.stat().st_mtime_ns)
        if stamp != _stamp:
            data = json.loads(INDEX.read_text(encoding='utf-8'))
            _catalog = [dict(e) for e in data.get('entries', [])]
            _stamp = stamp
    except (OSError, ValueError, TypeError):
        _catalog, _stamp = [], None
    return _catalog


def filtered(query):
    words = query.casefold().split()
    return [e for e in _catalog if all(word in (e['name']+' '+e.get('category','')).casefold()
                                       for word in words)]


def child_path(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('Library path is outside the built-in material directory')
    return path


def icon(entry):
    if _icons is None or entry['id'] not in _icons:
        return 0
    return _icons[entry['id']].icon_id


def _load_preview():
    global _icons
    if time.monotonic() < _preview_after:
        return max(.01, _preview_after - time.monotonic())
    if not _preview_queue:
        return None
    if _icons is None:
        _icons = bpy.utils.previews.new()
    entry = _preview_queue.pop(0)
    key = entry['id']
    if key not in _icons:
        try:
            path = child_path(PREVIEWS, entry['preview'])
            if path.is_file():
                _icons.load(key, str(path), 'IMAGE')
        except (OSError, RuntimeError, ValueError):
            pass
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':area.tag_redraw()
    return .01 if _preview_queue else None


def _queue_previews(state, delay=.15):
    global _preview_queue, _preview_after
    entries = filtered(state.search)
    page = min(state.page, max(0, (len(entries)-1)//PAGE_SIZE))
    _preview_queue = [e for e in entries[page*PAGE_SIZE:(page+1)*PAGE_SIZE]
                      if not icon(e)]
    _preview_after = time.monotonic() + delay
    if _preview_queue and not bpy.app.timers.is_registered(_load_preview):
        bpy.app.timers.register(_load_preview, first_interval=delay)


def target(operator):
    obj = bpy.data.objects.get(operator.target_name)
    if obj is None:
        raise ValueError('The material target was deleted or renamed; reopen the browser')
    owner = obj.path_resolve(operator.data_path) if operator.data_path else obj
    prop = owner.bl_rna.properties.get(operator.field)
    if prop is None or prop.type != 'POINTER' or prop.fixed_type.identifier != 'Material':
        raise ValueError('The selected property is not a material slot')
    if obj.library is not None:
        raise ValueError('The target is linked and cannot be edited')
    return owner


def append_material(entry):
    if not available():raise ValueError('Install the separate CC0 material library in Project Damage Preferences')
    path = child_path(LIBRARY, entry['blend'])
    if not path.is_file():
        raise ValueError('Material library file not found: '+path.name)
                                                                          
    for material in bpy.data.materials:
        if (material.get('project_damage_library_id') == entry['id']
                and material.get('project_damage_library_file') == str(path)
                and not material.get('project_damage_shader_uv')):
            return material
    with bpy.data.libraries.load(str(path), link=False) as (contents, loaded):
        if entry['name'] not in contents.materials:
            raise ValueError('Material not found in library: '+entry['name'])
        loaded.materials = [entry['name']]
    material = loaded.materials[0]
    if material is None or material.is_grease_pencil:
        raise ValueError('This is not a mesh material')
    visited=set()
    def repair(tree):
        if tree is None or tree.as_pointer() in visited:return
        visited.add(tree.as_pointer())
        for node in tree.nodes:
            image=getattr(node,'image',None)
            if image is not None and not image.packed_file and image.source=='FILE':
                filename=Path(bpy.path.abspath(image.filepath,library=image.library)).name
                candidate=path.parent/filename
                if not candidate.is_file():candidate=next(path.parent.rglob(filename),None)
                if candidate is not None and candidate.is_file():image.filepath=str(candidate)
            if node.type=='GROUP':repair(node.node_tree)
    repair(material.node_tree)
    material['project_damage_library_id'] = entry['id']
    material['project_damage_library_file'] = str(path)
    return material


# строка материала и кнопка его выбора из библиотеки
def draw_property(layout, owner, name, **kwargs):
    """Preserve Blender's normal selector/clear button and append a menu icon."""
    row = layout.row(align=True)
                                                                           
    raw = getattr(row, '__dict__', {}).get('layout', row)
    label=kwargs.pop('text',None) or owner.bl_rna.properties[name].name
    kwargs.pop('icon',None)
    split=raw.split(factor=.35,align=True)
    title=split.row(align=True);title.label(text=label,icon='MATERIAL')
    raw=split.row(align=True)
    raw.prop(owner,name,text='',**kwargs)
    obj = owner.id_data
    if not isinstance(obj, bpy.types.Object):
        return
    op = raw.operator('project_damage.browse_materials', text='', icon='COLLAPSEMENU')
    op.target_name = obj.name
    op.data_path = owner.path_from_id()
    op.field = name


def _search_changed(self, context):
    self.page = 0
    _queue_previews(self)
    if context and context.area:
        context.area.tag_redraw()
    if context and context.region:
        context.region.tag_redraw()


class ProjectDamageMaterialBrowserState(bpy.types.PropertyGroup):
    target_name: bpy.props.StringProperty()
    data_path: bpy.props.StringProperty()
    field: bpy.props.StringProperty()
    search: bpy.props.StringProperty(name='Search Materials', options={'TEXTEDIT_UPDATE'}, update=_search_changed)
    page: bpy.props.IntProperty(default=0, min=0)


class PROJECT_DAMAGE_PT_material_browser(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_ui_units_x = 32
    bl_label = 'Built-in Materials'

    def draw(self, context):
        self.layout.ui_units_x = 32.5
        self.layout.operator_context = 'EXEC_DEFAULT'
        _draw_browser(self.layout, context.window_manager.project_damage_material_browser, context)


# действие «Built-in Materials»
class PROJECT_DAMAGE_OT_browse_materials(bpy.types.Operator):
    bl_idname = 'project_damage.browse_materials'
    bl_label = 'Built-in Materials'
    bl_description = 'Browse the local Project Damage material library'
    bl_options = {'INTERNAL'}
    target_name: bpy.props.StringProperty(options={'HIDDEN'})
    data_path: bpy.props.StringProperty(options={'HIDDEN'})
    field: bpy.props.StringProperty(options={'HIDDEN'})
    search: bpy.props.StringProperty(name='Search Materials', options={'TEXTEDIT_UPDATE','SKIP_SAVE'})
    page: bpy.props.IntProperty(default=0, min=0, options={'SKIP_SAVE'})

    def invoke(self, context, event):
        try:
            target(self)
        except (ValueError, AttributeError) as exc:
            self.report({'WARNING'}, str(exc)); return {'CANCELLED'}
        global _browser_available
        _browser_available = available()
        catalog()
        state=context.window_manager.project_damage_material_browser
        for field in ('target_name','data_path','field','search','page'):
            setattr(state,field,getattr(self,field))
        _queue_previews(state, .01)
        # поиск меняет содержимое того же окна, без закрытия и нового открытия
        bpy.ops.wm.call_panel('INVOKE_DEFAULT',name='PROJECT_DAMAGE_PT_material_browser',keep_open=True)
        return {'FINISHED'}

    def execute(self, context):
        return {'FINISHED'}

    @staticmethod
    def draw_credits(layout):
        row = layout.row(align=True)
        row.alignment = 'LEFT'
        row.operator_context = 'INVOKE_DEFAULT'
        row.operator('project_damage.ambientcg_info', text='Info', icon='INFO')
        row.operator('wm.url_open', text='Donate to AmbientCG', icon='HEART').url = 'https://patreon.com/ambientcg'


class PROJECT_DAMAGE_OT_material_browser_page(bpy.types.Operator):
    bl_idname = 'project_damage.material_browser_page'
    bl_label = 'Material Library Page'
    bl_description = 'Show another page without moving the material browser'
    bl_options = {'INTERNAL'}
    delta: bpy.props.IntProperty(default=1, options={'HIDDEN', 'SKIP_SAVE'})

    def execute(self, context):
        state = context.window_manager.project_damage_material_browser
        last = max(0, (len(filtered(state.search)) - 1) // PAGE_SIZE)
        state.page = min(last, max(0, state.page + self.delta))
        _queue_previews(state, .01)
        if context.region:
            context.region.tag_redraw()
        return {'FINISHED'}

def _draw_browser(layout, self, context):
    layout.use_property_split = False
    row = layout.row(); row.alignment = 'CENTER'
    row.label(text='Built-in Materials', icon='MATERIAL')
    if not _browser_available:
        draw_missing_library(layout)
        PROJECT_DAMAGE_OT_browse_materials.draw_credits(layout)
        return
    layout.prop(self, 'search', text='', icon='VIEWZOOM')
    entries = filtered(self.search)
    pages = max(1, math.ceil(len(entries)/PAGE_SIZE))
    page = min(self.page, pages-1)
    layout.label(text=('%d materials — Page %d / %d' % (len(entries),page+1,pages))
                 if entries else 'No matching materials. Try another search.')
    grid = layout.grid_flow(row_major=True, columns=3, even_columns=True, even_rows=True, align=True)
    page_entries=entries[page*PAGE_SIZE:(page+1)*PAGE_SIZE]
    for index in range(PAGE_SIZE):
        entry=page_entries[index] if index<len(page_entries) else None
        box=grid.box()
        row=box.row();row.alignment='CENTER';row.ui_units_y=4.8
        if entry is None:
            box.enabled=False;row.label(text='');box.label(text='')
            continue
        value = icon(entry)
        if value:
            row.template_icon(icon_value=value, scale=4.8)
        else:
            row.label(text='', icon='MATERIAL')
        label = entry['name'].replace('_2K-JPG','').replace('_DISPLACEMENT',' · Displacement').replace('_NORMAL',' · Normal')
        # назначение закрывает окно, а поиск и листание оставляют его открытым
        select = box.template_popup_confirm('project_damage.choose_library_material',
                                            text=label, cancel_text='')
        select.entry_id = entry['id']
        select.target_name = self.target_name
        select.data_path = self.data_path
        select.field = self.field
    row = layout.row(align=True)
    for direction, label, delta, enabled in (('TRIA_LEFT','Previous',-1,page>0),('TRIA_RIGHT','Next',1,page+1<pages)):
        column = row.column(align=True); column.enabled = enabled
        change = column.operator('project_damage.material_browser_page', text=label, icon=direction)
        change.delta = delta
    layout.label(text='Select a material by its name to assign it and close.', icon='INFO')
    PROJECT_DAMAGE_OT_browse_materials.draw_credits(layout)


# действие «Thanks to ambientCG»
class PROJECT_DAMAGE_OT_ambientcg_info(bpy.types.Operator):
    bl_idname = 'project_damage.ambientcg_info'
    bl_label = 'Thanks to ambientCG'
    bl_description = 'About the materials supplied by ambientCG'
    bl_options = {'INTERNAL'}

    def invoke(self, context, event):
        return context.window_manager.invoke_popup(self, width=620)

    def execute(self, context):
        return {'FINISHED'}

    def draw(self, context):
        col = self.layout.column(align=True)
        col.label(text='Thank you, ambientCG,')
        col.label(text='for this excellent CC0 material library!')
        col.label(text='Visit their site to explore more of their CC0 assets.')
        col.separator()
        col.operator('wm.url_open', text='Visit ambientCG', icon='URL').url = 'https://ambientcg.com/'


# действие «Assign Built-in Material»
class PROJECT_DAMAGE_OT_choose_library_material(bpy.types.Operator):
    bl_idname = 'project_damage.choose_library_material'
    bl_label = 'Assign Built-in Material'
    bl_description = 'Append this built-in material and assign it to the chosen effect slot'
    bl_options = {'REGISTER','UNDO'}
    entry_id: bpy.props.StringProperty()
    target_name: bpy.props.StringProperty(options={'HIDDEN'})
    data_path: bpy.props.StringProperty(options={'HIDDEN'})
    field: bpy.props.StringProperty(options={'HIDDEN'})

    @classmethod
    def description(cls, context, properties):
        entry = next((e for e in _catalog if e['id']==properties.entry_id),None)
        return (entry['name']+'\n'+entry['blend']) if entry else cls.bl_description

    def execute(self, context):
        try:
            owner = target(self)
            entry = next((e for e in catalog() if e['id']==self.entry_id),None)
            if entry is None:
                raise ValueError('This material is no longer in the catalog')
            material = append_material(entry)
            from . import ownership,material_uv
            source=ownership.source_of(owner.id_data)
            obj=material_uv.target_object(context,source)
            if obj is not None:material=material_uv.for_object(material,obj)
            if isinstance(owner, bpy.types.MaterialSlot):
                from . import material_update
                with material_update.shader_edit(context):
                    setattr(owner, self.field, material)
            else:
                setattr(owner, self.field, material)
        except (ValueError, OSError, RuntimeError, AttributeError) as exc:
            self.report({'ERROR'}, str(exc)); return {'CANCELLED'}
        for screen in bpy.data.screens:
            for area in screen.areas:
                area.tag_redraw()
        return {'FINISHED'}


_CLASSES = (ProjectDamageMaterialBrowserState, PROJECT_DAMAGE_PT_material_browser, PROJECT_DAMAGE_OT_install_material_library, PROJECT_DAMAGE_OT_remove_material_library, PROJECT_DAMAGE_OT_ambientcg_info, PROJECT_DAMAGE_OT_browse_materials, PROJECT_DAMAGE_OT_material_browser_page, PROJECT_DAMAGE_OT_choose_library_material)


# подключаем свойства и действия этого модуля к Blender
def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.WindowManager.project_damage_material_browser = bpy.props.PointerProperty(type=ProjectDamageMaterialBrowserState)


# убираем подключения модуля при выключении аддона
def unregister():
    global _icons, _stamp
    if bpy.app.timers.is_registered(_load_preview):bpy.app.timers.unregister(_load_preview)
    _preview_queue.clear()
    if _icons is not None:
        bpy.utils.previews.remove(_icons)
        _icons = None
    _icon_stamps.clear(); _catalog.clear(); _stamp = None
    if hasattr(bpy.types.WindowManager, 'project_damage_material_browser'):
        del bpy.types.WindowManager.project_damage_material_browser
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
