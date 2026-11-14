# большое окно выбора пресетов с поиском и увеличением превью
# SPDX-License-Identifier: GPL-3.0-or-later
"""GPU preset selector; hover with Shift for a large, fitted image preview."""
import math
import bpy
from bpy.app.handlers import persistent
from . import gpu_ui
_ACTIVE={}

def advanced(context):
 entry=context.preferences.addons.get(__package__)
 return entry is not None and getattr(entry.preferences,'preset_ui','LEGACY')=='ADVANCED'

def draw_selector(layout,context):
 from . import presets
 item=presets.builtin_selected_item(context.window_manager)
 row=layout.row();row.alignment='CENTER'
 if item and item[3]:row.template_icon(icon_value=item[3],scale=6.)
 row=layout.row();row.scale_y=1.5
 row.operator('project_damage.preset_browser',text='Browse Presets',icon='IMAGE_DATA')
 layout.label(text='Hold Shift over a preview to enlarge it',icon='INFO')

def entries(category, query=''):
 from . import presets
 return [{'key':key, 'name':presets.presets_library.preset_title(key), 'path':presets.builtin_preview_path(group,key)}
         for group,key in presets.builtin_matches(category,query)]

# действие «Project Damage Presets»
class PROJECT_DAMAGE_OT_preset_browser(bpy.types.Operator):
 bl_idname='project_damage.preset_browser';bl_label='Project Damage Presets';bl_options={'INTERNAL'}
 bl_description='Select a preset; hold Shift over a thumbnail to enlarge it. Selection does not rebuild geometry'
 @classmethod
 def poll(cls,context):return context.area is not None and context.area.type=='VIEW_3D'
 def area(self):
  try:return next((a for a in self.window.screen.areas if a.as_pointer()==self.area_id),None)
  except ReferenceError:return None
 def finish(self):
  if self.closed:return {'FINISHED'}
  self.closed=True;_ACTIVE.pop(self.window_id,None)
  for handle,region_type in self.handles:
   try:bpy.types.SpaceView3D.draw_handler_remove(handle,region_type)
   except (ValueError,ReferenceError):pass
  self.handles=[];self.textures.clear()
  for image in self.loaded_images:
   try:
    if image.users==0:bpy.data.images.remove(image)
   except ReferenceError:pass
  self.loaded_images=[];self.images.clear();self.items=[];self.cells=[]
  gpu_ui.BROWSER_WINDOWS.discard(self.window_id)
  area=self.area()
  if area:area.tag_redraw()
  return {'FINISHED'}
 def refresh(self,context):
  self.items=entries(self.category,self.query)
  self.scroll=0;self.hover=-1
 def load_visible(self):
                                                                             
  paths={self.items[index]['path'] for index,box in self.cells}
  visible={str(path) for path in paths if path is not None}
  for token in tuple(self.images):
   if token in visible:continue
   self.textures.pop(token,None)
   image=self.images.pop(token)
   if image is not None and image in self.loaded_images:
    self.loaded_images.remove(image)
    try:
     if image.users==0:bpy.data.images.remove(image)
    except ReferenceError:pass
  pending=[path for path in paths if path is not None and str(path) not in self.images]
  if not pending:return
  previous={image.as_pointer() for image in bpy.data.images}
  for path in pending:
   token=str(path)
   try:
    image=bpy.data.images.load(token,check_existing=True)
   except (RuntimeError,OSError):
    self.images[token]=None
    continue
   self.images[token]=image
   if image.as_pointer() not in previous:
    self.loaded_images.append(image);previous.add(image.as_pointer())
 def update_view(self,context,area):
  if self.geometry(context,area) is None:return False
  self.load_visible()
  return True
 def geometry(self,context,area):
  s=gpu_ui.scale(context);region=next((r for r in area.regions if r.type=='WINDOW'),None)
  if region is None:return None
  bottom=region.y-area.y+12*s;top=region.y-area.y+region.height-12*s
  for r in area.regions:
   if r.width<=1 or r.height<=1:continue
   if r.type in {'ASSET_SHELF','ASSET_SHELF_HEADER'}:bottom=max(bottom,r.y-area.y+r.height+12*s)
   if r.type in {'HEADER','TOOL_HEADER'} and r.y>region.y+region.height*.5:top=min(top,r.y-area.y-8*s)
  w=min(920*s,area.width-24*s);h=min(710*s,top-bottom)
  if w<150*s or h<150*s:return None
  x=max(12*s,min(self.origin_x-w*.5,area.width-w-12*s));y=bottom+(top-bottom-h)*.5
  pad=14*s;head=88*s;foot=30*s;gap=10*s
  columns=max(1,int((w-2*pad+gap)/(154*s+gap)));cell=(w-2*pad-(columns-1)*gap)/columns
  row_h=cell+30*s;visible=max(1,int((h-head-foot)/(row_h+gap)))
  self.max_scroll=max(0,math.ceil(len(self.items)/columns)-visible);self.scroll=min(self.scroll,self.max_scroll)
  self.cells=[]
  for k in range(visible*columns):
   index=self.scroll*columns+k
   if index>=len(self.items):break
   cx=x+pad+(k%columns)*(cell+gap);cy=y+h-head-(k//columns+1)*(row_h+gap)
   if cy<y+foot:continue
   self.cells.append((index,(cx,cy,cell,row_h)))
  self.panel=(x,y,w,h);self.search=(x+pad,y+h-76*s,w-2*pad,30*s)
  self.previous=(x+w-74*s,y+h-35*s,26*s,24*s);self.next=(x+w-42*s,y+h-35*s,26*s,24*s)
  return s
 def draw_canvas(self):
  context=bpy.context;area=self.area()
  if area is None or context.area!=area or context.region.type not in {'WINDOW','UI','TOOLS'}:return
  s=self.geometry(context,area)
  if s is None:return
  import gpu
  old=gpu.state.blend_get();gpu.state.blend_set('ALPHA')
  dx=area.x-context.region.x;dy=area.y-context.region.y
  def rect(box,color):gpu_ui.rect(box[0]+dx,box[1]+dy,box[2],box[3],color,6*s)
  def text(value,x,y,size=13,color=(.94,.95,.97,1),width=None):gpu_ui.text(value,x+dx,y+dy,size*s,color,width)
  def preview(item,box):
   token=str(item['path']);im=self.images.get(token)
   if im is None:return
   try:
    if token not in self.textures:self.textures[token]=gpu.texture.from_image(im)
    gpu_ui.image(self.textures[token],box[0]+dx,box[1]+dy,box[2],box[3])
   except (ReferenceError,RuntimeError):pass
  try:
   x,y,w,h=self.panel;rect(self.panel,(.055,.065,.08,.985))
   from . import presets
   title='Search results' if self.query.strip() else presets.builtin_catalog().get(self.category,('Presets',))[0]
   text('Project Damage — '+title,x+14*s,y+h-28*s,15,width=w-110*s)
   if not self.query.strip():
    rect(self.previous,(.14,.16,.19,1));rect(self.next,(.14,.16,.19,1))
    text('‹',self.previous[0]+8*s,self.previous[1]+5*s,15);text('›',self.next[0]+8*s,self.next[1]+5*s,15)
   rect(self.search,(.12,.16,.2,1) if self.typing else (.10,.115,.14,1))
   text(self.query+('|' if self.typing else '') if self.query else 'Search presets…',self.search[0]+9*s,self.search[1]+9*s,12,width=self.search[2]-18*s)
   selection=presets.builtin_selected(context.window_manager)
   selected=selection[1] if selection else None
   for index,box in self.cells:
    item=self.items[index];highlight=index==self.hover or item['key']==selected
    rect(box,(.16,.30,.46,1) if highlight else (.095,.105,.125,1))
    bx,by,bw,bh=box;preview(item,(bx+5*s,by+29*s,bw-10*s,bh-34*s))
    text(item['name'],bx+7*s,by+10*s,12,width=bw-14*s)
   if not self.items:text('No matching presets',x+20*s,y+h*.5,14)
   text('Click to select • Shift: enlarge • Wheel: scroll • Esc: close',x+14*s,y+11*s,11,width=w-28*s)
   if self.shift and not self.typing and 0<=self.hover<len(self.items):
    item=self.items[self.hover];zoom=min(420*s,w-30*s,h-70*s)
    zx=max(x+10*s,min(self.mouse[0]+18*s,x+w-zoom-10*s));zy=max(y+35*s,min(self.mouse[1]-zoom*.5,y+h-zoom-40*s))
    rect((zx-5*s,zy-29*s,zoom+10*s,zoom+34*s),(.2,.4,.65,1))
    rect((zx,zy-25*s,zoom,zoom+25*s),(.055,.065,.08,1))
    preview(item,(zx,zy,zoom,zoom));text(item['name'],zx+7*s,zy-17*s,13,width=zoom-14*s)
  finally:gpu.state.blend_set(old)
 def invoke(self,context,event):
  if context.window.as_pointer() in gpu_ui.BROWSER_WINDOWS:return {'CANCELLED'}
  self.window=context.window;self.window_id=context.window.as_pointer();self.area_id=context.area.as_pointer()
  self.origin_x=event.mouse_x-context.area.x;self.query=context.window_manager.project_damage_builtin_search;self.typing=False;self.shift=False;self.scroll=0;self.hover=-1;self.mouse=(self.origin_x,event.mouse_y-context.area.y)
  self.images={};self.loaded_images=[];self.textures={};self.cells=[];self.handles=[];self.closed=False
  self.category=context.window_manager.project_damage_builtin_category;self.refresh(context)
  if not self.update_view(context,context.area):return self.finish()
  for region_type in ('WINDOW','UI','TOOLS'):
   self.handles.append((bpy.types.SpaceView3D.draw_handler_add(self.draw_canvas,(),region_type,'POST_PIXEL'),region_type))
  _ACTIVE[self.window_id]=self;gpu_ui.BROWSER_WINDOWS.add(self.window_id);context.window_manager.modal_handler_add(self);context.area.tag_redraw();return {'RUNNING_MODAL'}
 def modal(self,context,event):
  if self.closed:return {'CANCELLED'}
  area=self.area()
  if area is None:return self.finish()
  if not self.update_view(context,area):return self.finish()
  old_hover,old_shift,old_mouse=self.hover,self.shift,self.mouse
  self.mouse=(event.mouse_x-area.x,event.mouse_y-area.y);self.shift=event.shift
  self.hover=next((i for i,box in self.cells if gpu_ui.contains(box,*self.mouse)),-1)
  if self.hover!=old_hover or self.shift!=old_shift or (self.shift and self.hover>=0 and self.mouse!=old_mouse):area.tag_redraw()
  if event.type in {'ESC','RIGHTMOUSE'} and event.value=='PRESS':return self.finish()
  if self.typing and event.value=='PRESS' and event.type not in {'LEFTMOUSE','MIDDLEMOUSE','WHEELUPMOUSE','WHEELDOWNMOUSE'}:
   old_query=self.query
   if event.type=='BACK_SPACE':self.query='' if event.ctrl or event.oskey else self.query[:-1]
   elif event.type in {'RET','NUMPAD_ENTER','TAB'}:self.typing=False
   elif event.ascii and event.ascii.isprintable() and not (event.ctrl or event.oskey):self.query+=event.ascii
   if self.query!=old_query:
    context.window_manager.project_damage_builtin_search=self.query
    self.refresh(context);self.update_view(context,area)
   if not self.typing or self.query!=old_query:area.tag_redraw()
   return {'RUNNING_MODAL'}
  if event.type in {'WHEELUPMOUSE','WHEELDOWNMOUSE'} and event.value=='PRESS':
   self.scroll=max(0,min(self.max_scroll,self.scroll+(-1 if event.type=='WHEELUPMOUSE' else 1)))
   self.update_view(context,area);area.tag_redraw();return {'RUNNING_MODAL'}
  if event.type=='LEFTMOUSE' and event.value=='PRESS':
   area.tag_redraw()
   if gpu_ui.contains(self.search,*self.mouse):self.typing=True;return {'RUNNING_MODAL'}
   self.typing=False
   if not self.query.strip() and (gpu_ui.contains(self.previous,*self.mouse) or gpu_ui.contains(self.next,*self.mouse)):
    from . import presets
    keys=sorted(presets.builtin_catalog());step=-1 if gpu_ui.contains(self.previous,*self.mouse) else 1
    self.category=keys[(keys.index(self.category)+step)%len(keys)];context.window_manager.project_damage_builtin_category=self.category;self.refresh(context);self.update_view(context,area);return {'RUNNING_MODAL'}
   if self.hover>=0:
    from . import presets
    presets.select_builtin(context.window_manager,self.items[self.hover]['key']);return self.finish()
   if not gpu_ui.contains(self.panel,*self.mouse):return self.finish()
  return {'RUNNING_MODAL'}
 def cancel(self,context):self.finish()

@persistent
def close_browsers(*_):
 for op in list(_ACTIVE.values()):op.finish()
 _ACTIVE.clear()

# подключаем свойства и действия этого модуля к Blender
def register():
 bpy.utils.register_class(PROJECT_DAMAGE_OT_preset_browser)
 if close_browsers not in bpy.app.handlers.load_pre:bpy.app.handlers.load_pre.append(close_browsers)
# убираем подключения модуля при выключении аддона
def unregister():
 close_browsers()
 if close_browsers in bpy.app.handlers.load_pre:bpy.app.handlers.load_pre.remove(close_browsers)
 bpy.utils.unregister_class(PROJECT_DAMAGE_OT_preset_browser)
