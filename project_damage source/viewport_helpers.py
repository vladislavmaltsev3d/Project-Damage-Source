# быстрые кнопки во вьюпорте и состояние фоновых задач
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-blocking viewport helpers scoped to the visible Project Damage sidebar."""
import time
from dataclasses import dataclass
import bpy
from bpy.app.handlers import persistent
from . import gpu_ui, viewport_icons

_enabled=False
_handle=None
_windows=set()
_modal_owner={}
_hover={}
_tooltip_shown={}
_hit_regions={}
_token=object()
_display_pending=False
_update_feedback={}
_UPDATE_PRESS_DELAY=.04
_UPDATE_FADE_DURATION=.15


@dataclass
class _UpdatePress:
 scene_uid: int
 source_uid: int
 run_at: float
 released_at: float | None = None

_pointer_fields={}
_GP_TYPES={'GREASEPENCIL','GPENCIL'}
_render_passes = {}

def _fields(settings,groups=False):
                                                                               
                                                                           
 key=(settings.bl_rna.as_pointer(),groups)
 if key not in _pointer_fields:
  _pointer_fields[key]=tuple(p.identifier for p in settings.bl_rna.properties
   if p.type=='POINTER' and ((p.identifier.startswith('project_damage') or p.identifier=='pd_structure') if groups
                            else p.fixed_type.identifier=='Object'))
 return _pointer_fields[key]

def linked_drawings(owner):
 """Only GP inputs assigned to this source, including nested asset settings."""
 found={}
 if owner is None:return []
 for group in _fields(owner,groups=True):
  settings=getattr(owner,group,None)
  if not isinstance(settings,bpy.types.PropertyGroup):continue
  for name in _fields(settings):
   drawing=getattr(settings,name,None)
   if isinstance(drawing,bpy.types.Object) and drawing.type in _GP_TYPES:
    found[drawing.as_pointer()]=drawing
 return list(found.values())

def drawings():
 found={}
 for ob in bpy.data.objects:
  if ob.type in _GP_TYPES and ob.get('project_damage_paint_source'):found[ob.as_pointer()]=ob
                                                                           
                                                                   
 for owner in bpy.data.objects:
  for drawing in linked_drawings(owner):found[drawing.as_pointer()]=drawing
 return list(found.values())


@persistent
def prepare_drawing_render(scene):
    """Exclude guide strokes while providing the passes Blender may still request."""
    helpers = {obj for obj in scene.objects
               if obj.type in _GP_TYPES and obj.get('project_damage_paint_source')}
    for owner in scene.objects:
        helpers.update(linked_drawings(owner))
    if not helpers:
        return
    for obj in helpers:
        if obj.is_editable and not obj.hide_render:
            obj.hide_render = True
    passes = _render_passes.setdefault(scene.session_uid, [])
    for layer in scene.view_layers:
        if not getattr(layer, 'use_grease_pencil', True):
            continue
        for name in ('use_pass_combined', 'use_pass_z'):
            if not getattr(layer, name):
                setattr(layer, name, True)
                passes.append((layer, name))


def _restore_drawing_passes(passes):
    for layer, name in passes:
        try:
            setattr(layer, name, False)
        except ReferenceError:
            pass


@persistent
def finish_drawing_render(scene):
    _restore_drawing_passes(_render_passes.pop(scene.session_uid, ()))


_RENDER_HANDLERS = (('render_init', prepare_drawing_render),
                    ('render_complete', finish_drawing_render),
                    ('render_cancel', finish_drawing_render))

def has_drawings(context,owner):
 if linked_drawings(owner):return True
                                                                         
 active=context.active_object
 return bool(active is not None and active.type in _GP_TYPES
             and active.get('project_damage_paint_source')==owner)

def display(context=None,objects=None):
 context=context or bpy.context;wm=context.window_manager
 alpha=0. if wm.project_damage_drawings_hidden else wm.project_damage_drawing_opacity
 for ob in drawings() if objects is None else objects:
                                                                                     
  if ob.name in context.view_layer.objects and ob.hide_get()!=wm.project_damage_drawings_hidden:ob.hide_set(wm.project_damage_drawings_hidden)
  for i,mat in enumerate(ob.data.materials):
   if not mat or not mat.is_grease_pencil:continue
   if mat.users>1 and any(other.type in {'GREASEPENCIL','GPENCIL'} and not other.get('project_damage_paint_source') and mat in other.data.materials[:] for other in bpy.data.objects):
    mat=mat.copy();ob.data.materials[i]=mat
   rgb=mat.grease_pencil.color[:3];mat.grease_pencil.color=(*rgb,alpha)
 for window in wm.windows:
  for area in window.screen.areas:
   if area.type=='VIEW_3D':area.tag_redraw()

def activate(context,obj):
 context.window_manager.project_damage_drawings_hidden=False
 display(context)

def source(context):
 from . import sidebar,structure_surface
 standalone=structure_surface.owner(context)
 if standalone is not None:return standalone
 obj=sidebar.source(context)
                                                                            
                                                                               
 if obj is not None and obj.get('project_damage_uid'):return obj

def live_settings(obj):
 return obj.pd_structure if obj.get('pd_structure_source') else obj.project_damage

def _press_update(context,area):
 obj=source(context)
 if obj is None:return
 target=(context.window.as_pointer(),area.as_pointer())
 current=_update_feedback.get(target)
 if current and current.released_at is None:return
 _update_feedback[target]=_UpdatePress(context.scene.session_uid,obj.session_uid,time.monotonic()+_UPDATE_PRESS_DELAY)
 area.tag_redraw()
 if not bpy.app.timers.is_registered(_tick_update_feedback):
  bpy.app.timers.register(_tick_update_feedback,first_interval=_UPDATE_PRESS_DELAY)

def _tick_update_feedback():
 context=bpy.context
 windows={window.as_pointer():window for window in context.window_manager.windows}
 for target,press in tuple(_update_feedback.items()):
  window=windows.get(target[0])
  area=next((area for area in window.screen.areas
             if area.type=='VIEW_3D' and area.as_pointer()==target[1]),None) if window else None
  if area is None:
   del _update_feedback[target]
   continue
  now=time.monotonic()
  if press.released_at is None and now>=press.run_at:
   region=next((region for region in area.regions if region.type=='WINDOW'),None)
   try:
    if region and window.scene.session_uid==press.scene_uid:
     with context.temp_override(window=window,area=area,region=region):
      obj=source(context)
      if obj is not None and obj.session_uid==press.source_uid:
       bpy.ops.project_damage.helper_update()
   except (RuntimeError,ReferenceError):
    pass
   finally:
    press.released_at=time.monotonic()
  elif press.released_at is not None and now>=press.released_at+_UPDATE_FADE_DURATION:
   del _update_feedback[target]
  area.tag_redraw()
 return .033 if _update_feedback else None

def _clear_update_feedback():
 _update_feedback.clear()
 if bpy.app.timers.is_registered(_tick_update_feedback):
  bpy.app.timers.unregister(_tick_update_feedback)

def physics_tools(context,obj):
 """Match the selected effect's actual Physics tab and drawing controls."""
 if obj is None or obj.get('pd_structure_source'):return False,False
 from . import sidebar,physics_workflow
 key=physics_workflow.selected_key(obj)
 physics=(key in sidebar.PHYSICS_TAB_KEYS and key not in {'piles','deposits'}
          and key not in sidebar.REMOVED_EFFECTS
          and physics_workflow.supported(context,obj))
                                                                       
 from . import material_controls
 knives=physics and key!='dbevel' and material_controls.ui_section(context)=='PHYSICS'
 return physics,knives

def buttons(context,area,region):
 from . import background_compute as bg
 if context.window.as_pointer() in gpu_ui.BROWSER_WINDOWS:return []
 s=gpu_ui.scale(context);note=bg.notice(context.scene);items=[]
 if note:items.append(('BG_'+note[1],36*s))
 if gpu_ui.sidebar_visible(area):
  obj=source(context)
  if obj is not None:
   standalone=bool(obj.get('pd_structure_source'))
   manual=not live_settings(obj).live_update
   from . import physics_workflow
   _,knives=physics_tools(context,obj)
   if has_drawings(context,obj):items.append(('DRAWINGS',154*s))
   items += [('LIVE',36*s)]+([] if standalone else [('COMPUTE',36*s)])+([('UPDATE',92*s)] if manual else [])
   if knives:
    items += [('CUT_THROUGH',112*s),('CUT_SURFACE',112*s),('CUT_GUIDANCE',100*s)]
    module=physics_workflow.active_module(context,obj)
    if (obj.get('project_damage_quick_physics_painting')
        and obj.get('project_damage_quick_physics_module',module)==module):
     items.append(('PHYSICS_FINISH',155*s))
 if not items:return []
 widths=[w for _,w in items];gap=5*s;h=32*s
 x,y,available=gpu_ui.helper_origin(area,region,sum(widths)+gap*(len(widths)-1),h,12*s)
 if available<(36 if note else 120)*s:return []
 factor=min(1,available/(sum(widths)+gap*(len(widths)-1)))
 result=[]
 for key,w in items:
  result.append((key,(x,y,w*factor,h)));x+=w*factor+gap
 return result

def _icon(kind,x,y,size,active=False):
 viewport_icons.draw(kind,x,y,size,active)


# собираем видимые настройки этого раздела
def draw():
 context=bpy.context
 if not context.area or not context.region:return
 obj=context.object
 if obj is not None and context.mode in {'PAINT_GREASE_PENCIL','PAINT_GPENCIL'} and obj.get('project_damage_draw_hint'):
  import blf
  blf.size(0,14);blf.color(0,1.,.8,.3,1.);blf.position(0,80,context.region.height-100,0)
  blf.draw(0,obj['project_damage_draw_hint'])
 rows=buttons(context,context.area,context.region)
 target=(context.window.as_pointer(),context.area.as_pointer())
 if not rows:
  _hit_regions.pop(target,None)
  return
 _hit_regions[target]=rows
 import gpu
 old=gpu.state.blend_get();gpu.state.blend_set('ALPHA')
 try:
  wm=context.window_manager;obj=source(context) if any(not k.startswith('BG_') for k,_ in rows) else None;hover=_hover.get(context.window.as_pointer(),('',0,0,0));s=gpu_ui.scale(context)
  for key,(x,y,w,h) in rows:
   x-=context.region.x;y-=context.region.y
   active=(not wm.project_damage_drawings_hidden) if key=='DRAWINGS' else (obj is not None and live_settings(obj).live_update) if key=='LIVE' else bool(obj and obj.project_damage.background_compute) if key=='COMPUTE' else (obj is not None and (obj.project_damage.cellf_curve_type==key[4:]) and context.mode=='PAINT_GREASE_PENCIL') if key.startswith('CUT_') else False
   color=(.14,.32,.48,.94) if active else (.10,.115,.135,.94)
   if key.startswith('BG_'):color={'BG_RUNNING':(.21,.34,.56,.95),'BG_READY':(.12,.38,.21,.97),'BG_ERROR':(.53,.17,.13,.97)}[key]
   if hover[0]==key:color=tuple(min(1,v+.075) for v in color[:3])+(color[3],)
   button_scale=s
   press=_update_feedback.get(target) if key=='UPDATE' else None
   if press:
    strength=1 if press.released_at is None else max(0,1-(time.monotonic()-press.released_at)/_UPDATE_FADE_DURATION)
    growth=.045*strength
    x-=w*growth/2;y-=h*growth/2;w*=1+growth;h*=1+growth
    button_scale*=1+growth
    color=tuple(base+(highlight-base)*strength for base,highlight in zip(color,(.24,.49,.76,.98)))
   gpu_ui.rect(x,y,w,h,color,6*button_scale)
   label=('Hide Drawings' if active else 'Show Drawings') if key=='DRAWINGS' else ('Live Update' if active else 'Manual Update') if key=='LIVE' else 'Through' if key=='CUT_THROUGH' else 'Surface' if key=='CUT_SURFACE' else 'Guidance' if key=='CUT_GUIDANCE' else 'Fracture & Physics' if key=='PHYSICS_FINISH' else 'Update'
   if key=='DRAWINGS':label+='  '+str(round(wm.project_damage_drawing_opacity*100))+'%'
   if key.startswith('BG_') or key in {'LIVE','COMPUTE','CUT_THROUGH','CUT_SURFACE'}:
    _icon(key,x+6*s,y+6*s,20*s,active)
    if key.startswith('CUT_'):gpu_ui.text(label,x+32*s,y+10*s,12*s,width=w-37*s)
   else:gpu_ui.text(label,x+9*button_scale,y+10*button_scale,12*button_scale,width=w-18*button_scale,color=(.94,.95,.96,1) if key=='DRAWINGS' or obj else (.5,.5,.5,1))
  if hover[0] in {k for k,_ in rows} and time.monotonic()-hover[1]>.6:
   message={'BG_RUNNING':'Operation in progress. Click to open Project Damage for this object.', 'BG_READY':'Operation complete. Click to frame the object and open Project Damage.', 'BG_ERROR':'Calculation failed. Click to inspect the message in Project Damage.', 'DRAWINGS':'Click: show / hide Project Damage drawings. Wheel: opacity.','LIVE':'Toggle Live Update. Enabled: rebuild automatically. Disabled: use the Update button.','COMPUTE':'Background Calculation. Right-click when on to set the CPU budget.','UPDATE':'Rebuild geometry now, keeping the current drawing active.','PHYSICS_FINISH':'Apply all three drawing layers to prepared fills, then enable rigid-body physics.','CUT_THROUGH':'Knife Through: draw a knife through the object. Later crossing cuts stop at earlier cuts.','CUT_SURFACE':'Split only pieces directly touched by the stroke. Deeper rows stay intact.','CUT_GUIDANCE':'Draw a density guide: smaller pieces near the stroke; no exact cut.'}[hover[0]]
   x=rows[0][1][0]-context.region.x;y=rows[0][1][1]-context.region.y+40*s;w=min(context.region.width-24*s,len(message)*6.2*s)
   gpu_ui.rect(x,y,w,27*s,(.025,.03,.04,.98),4*s);gpu_ui.text(message,x+8*s,y+8*s,11*s,width=w-16*s)
 finally:gpu.state.blend_set(old)

def open_operation(context,area):
 from . import background_compute as bg,build
 note=bg.notice(context.scene)
 if note is None:return
 key,state,message=note;owner=bg.notice_source(key)
 if not bg.valid_id(owner):return
 space=area.spaces.active
 space.show_region_ui=True
                                                          
                                                                         
 def show_category():
  region=next((r for r in area.regions if r.type=='UI'),None)
  if region:region.active_panel_category='⚒ Project Damage'
  return bool(region and region.active_panel_category in {'project_damage','⚒ Project Damage'})
 try:opened=show_category()
 except (AttributeError,ReferenceError,RuntimeError,TypeError):opened=False
 if not opened:
  tries=[15];token=_token
  def after_draw():
   tries[0]-=1
   if not _enabled or token is not _token:return None
   try:
    if area.type!='VIEW_3D' or show_category():return None
    area.tag_redraw()
   except (AttributeError,RuntimeError,TypeError,ReferenceError):pass
   return .06 if tries[0]>0 else None
  bpy.app.timers.register(after_draw,first_interval=.04)
 if state=='READY' and space.region_3d:
  from mathutils import Vector
  target=None
  if key[1].startswith('generate_'):
   from . import generated_assets
   target=generated_assets.output(owner,key[1][9:])
  elif key[1] in {'grow_ivy','grow_roots'}:
   target=getattr(owner,'project_damage_'+key[1][5:]).result
  target=target or build.result_of(owner) or owner
  corners=[target.matrix_world@Vector(v) for v in target.bound_box]
  center=sum(corners,Vector())/len(corners)
  rv=space.region_3d
  if rv.view_perspective=='CAMERA':rv.view_perspective='PERSP'
  rv.view_location=center
  rv.view_distance=max(.02,max((p-center).length for p in corners)*2.5)
  bg.dismiss(key)
 if state=='ERROR':
  context.window_manager.popup_menu(lambda self,ctx:self.layout.label(text=message.splitlines()[-1][:180]),title='Project Damage',icon='ERROR')
  bg.dismiss(key)
 area.tag_redraw()

# действие «Update Project Damage»
class PROJECT_DAMAGE_OT_helper_update(bpy.types.Operator):
 bl_idname='project_damage.helper_update';bl_label='Update Project Damage';bl_options={'UNDO','INTERNAL'}
 @classmethod
 def poll(cls,context):return source(context) is not None
 def execute(self,context):
  from . import build,live,incremental,structure_surface
  obj=source(context)
  if live._HISTORY_RESTORING or live._APPLYING:return {'CANCELLED'}
  if obj.get('pd_structure_source'):
   if structure_surface._history_restoring or structure_surface._busy:return {'CANCELLED'}
   try:
    structure_surface.rebuild(obj)
    result=structure_surface.output(obj)
   except (RuntimeError,ValueError) as exc:
    self.report({'ERROR'},str(exc));return {'CANCELLED'}
   return {'FINISHED'} if result is not None else {'CANCELLED'}
  previous=live._APPLYING;live._APPLYING=True
  incremental.invalidate_body(obj)
  try:result,reports=build.rebuild_object(context,obj)
  finally:live._APPLYING=previous
  for flags,message in reports:self.report(flags,message)
  return {'FINISHED'} if result is not None else {'CANCELLED'}

class PROJECT_DAMAGE_OT_background_budget(bpy.types.Operator):
 bl_idname='project_damage.background_budget';bl_label='Background CPU Budget';bl_options={'INTERNAL'}
 def invoke(self,context,event):
  return context.window_manager.invoke_popup(self,width=270)
 def draw(self,context):
  entry=context.preferences.addons.get(__package__)
  if entry:
   self.layout.prop(entry.preferences,'background_cpu_budget',slider=True)
 def execute(self,context):
  return {'FINISHED'}

# действие «Project Damage Helpers»
class PROJECT_DAMAGE_OT_viewport_helpers(bpy.types.Operator):
 bl_idname='project_damage.viewport_helpers';bl_label='Project Damage Helpers';bl_options={'INTERNAL'}
 def invoke(self,context,event):
  self.window=context.window.as_pointer();self.token=_token;self.pressed=None
  if self.window in _windows:return {'CANCELLED'}
  self.owner=_modal_owner[self.window]=object()
  _windows.add(self.window);context.window_manager.modal_handler_add(self);return {'RUNNING_MODAL'}
 def modal(self,context,event):
  if not _enabled or self.token is not _token:
   if self.token is _token:_windows.discard(self.window);_hover.pop(self.window,None)
   return {'CANCELLED'}
  # tick() released this window (sidebar closed, nothing running): stop listening
  # so no Python runs per mouse move and Blender's autosave is not held back.
  if _modal_owner.get(self.window) is not self.owner:return {'CANCELLED'}
  from . import background_compute as bg
  bg.user_input(context.window,event)
  if event.type=='INBETWEEN_MOUSEMOVE':return {'PASS_THROUGH'}
  if event.type==self.pressed and event.value=='RELEASE':
   self.pressed=None;return {'RUNNING_MODAL'}
  if event.type not in {'MOUSEMOVE','INBETWEEN_MOUSEMOVE','LEFTMOUSE','RIGHTMOUSE','WHEELUPMOUSE','WHEELDOWNMOUSE'}:
   return {'PASS_THROUGH'}
  area=next((a for a in context.window.screen.areas if a.type=='VIEW_3D' and a.x<=event.mouse_x<a.x+a.width and a.y<=event.mouse_y<a.y+a.height),None)
  region=next((r for r in area.regions if r.type=='WINDOW'),None) if area else None
  with context.temp_override(area=area,region=region) if area and region else _noop():
   rows=_hit_regions.get((self.window,area.as_pointer()),()) if area and region else ()
   if area and not gpu_ui.sidebar_visible(area):rows=[row for row in rows if row[0].startswith('BG_')]
   if rows and event.value=='PRESS' and event.type!='MOUSEMOVE':
    rows=buttons(context,area,region)
   key=next((key for key,box in rows if gpu_ui.contains(box,event.mouse_x,event.mouse_y)),'')
   old=_hover.get(self.window,('',0,0,0));_hover[self.window]=(key,old[1] if key==old[0] else time.monotonic(),event.mouse_x,event.mouse_y)
   if area and key!=old[0]:area.tag_redraw()
   if not key:return {'PASS_THROUGH'}
   wm=context.window_manager
   if key=='DRAWINGS' and event.type in {'WHEELUPMOUSE','WHEELDOWNMOUSE'} and event.value=='PRESS':
    wm.project_damage_drawing_opacity=max(.05,min(1,round(wm.project_damage_drawing_opacity+(0.05 if event.type=='WHEELUPMOUSE' else -.05),3)));display(context);return {'RUNNING_MODAL'}
   if key=='COMPUTE' and event.type=='RIGHTMOUSE' and event.value=='PRESS':
    obj=source(context)
    if obj and obj.project_damage.background_compute and context.preferences.addons.get(__package__):
     self.pressed=event.type
     bpy.ops.project_damage.background_budget('INVOKE_DEFAULT')
     return {'RUNNING_MODAL'}
   if event.type=='LEFTMOUSE' and event.value=='PRESS':
    self.pressed=event.type
    if key.startswith('BG_'):open_operation(context,area)
    elif key=='DRAWINGS':wm.project_damage_drawings_hidden=not wm.project_damage_drawings_hidden;display(context)
    elif key=='LIVE':
     obj=source(context)
     if obj:
      settings=live_settings(obj)
      settings.live_update=not settings.live_update
      area.tag_redraw()
    elif key=='COMPUTE':
     obj=source(context)
     if obj:
      obj.project_damage.background_compute=not obj.project_damage.background_compute
      area.tag_redraw()
    elif key=='PHYSICS_FINISH' and source(context):bpy.ops.project_damage.finish_physics_paint()
    elif key=='UPDATE':_press_update(context,area)
    elif key.startswith('CUT_') and source(context):bpy.ops.project_damage.cell_cut_draw(mode=key[4:])
    return {'RUNNING_MODAL'}
  return {'PASS_THROUGH'}

from contextlib import nullcontext as _noop

def tick():
 global _display_pending
 if not _enabled:return None
                                                                       
                                                                   
 if not hasattr(bpy.data,'objects') or not getattr(bpy.context,'view_layer',None):return .3
 if _display_pending:
  display()
  _display_pending=False
 animating=False
 windows=bpy.context.window_manager.windows
 valid={window.as_pointer() for window in windows}
 _windows.intersection_update(valid)
 for ptr in tuple(_modal_owner):
  if ptr not in valid:_modal_owner.pop(ptr,None)
 for ptr in set(_hover)|set(_tooltip_shown):
  if ptr not in valid:_hover.pop(ptr,None);_tooltip_shown.pop(ptr,None)
 areas={(w.as_pointer(),a.as_pointer()) for w in windows for a in w.screen.areas if a.type=='VIEW_3D'}
 for key in tuple(_hit_regions):
  if key not in areas:_hit_regions.pop(key,None)
 for window in windows:
  ptr=window.as_pointer()
  from . import background_compute as bg
  note=bg.notice(window.scene)
  if not note and not any(gpu_ui.sidebar_visible(a) for a in window.screen.areas):
   _hover.pop(ptr,None);_tooltip_shown.pop(ptr,None)
   from . import background_compute as _bg
   if ptr in _windows and not _bg._jobs:_windows.discard(ptr);_modal_owner.pop(ptr,None)
   for key in tuple(_hit_regions):
    if key[0]==ptr:_hit_regions.pop(key,None)
  if note and note[1]=='RUNNING':
   animating=True
   for area in window.screen.areas:
    if area.type=='VIEW_3D':area.tag_redraw()
  if ptr not in _windows and any(a.type=='VIEW_3D' and (note or gpu_ui.sidebar_visible(a)) for a in window.screen.areas):
   with bpy.context.temp_override(window=window):bpy.ops.project_damage.viewport_helpers('INVOKE_DEFAULT')
  hover=_hover.get(ptr,('',0,0,0))
  if hover[0] and time.monotonic()-hover[1]>.6 and _tooltip_shown.get(ptr)!=hover[1]:
   _tooltip_shown[ptr]=hover[1]
   for area in window.screen.areas:
    if area.type=='VIEW_3D' and (note or gpu_ui.sidebar_visible(area)):area.tag_redraw()
 return .05 if animating else .3

@persistent
def loaded(*_):
 global _token,_display_pending
 _clear_update_feedback()
 gpu_ui.clear_batches()
 viewport_icons.clear()
 _pointer_fields.clear()
 _render_passes.clear()
 _token=object();_windows.clear();_modal_owner.clear();_hover.clear();_tooltip_shown.clear();_hit_regions.clear()
 _display_pending=True

CLASSES=(PROJECT_DAMAGE_OT_helper_update,PROJECT_DAMAGE_OT_background_budget,PROJECT_DAMAGE_OT_viewport_helpers)
# подключаем свойства и действия этого модуля к Blender
def register():
 global _enabled,_handle,_token,_display_pending
 viewport_icons.preload()
 for cls in CLASSES:bpy.utils.register_class(cls)
 bpy.types.WindowManager.project_damage_drawings_hidden=bpy.props.BoolProperty(default=False,options={'SKIP_SAVE'})
 bpy.types.WindowManager.project_damage_drawing_opacity=bpy.props.FloatProperty(default=.85,min=.05,max=1,options={'SKIP_SAVE'})
 _pointer_fields.clear()
 _token=object();_windows.clear();_modal_owner.clear();_hover.clear();_tooltip_shown.clear();_hit_regions.clear();_enabled=True;_display_pending=True
 if not bpy.app.background:
  _handle=bpy.types.SpaceView3D.draw_handler_add(draw,(),'WINDOW','POST_PIXEL')
  bpy.app.timers.register(tick,first_interval=.5,persistent=True)
 if loaded not in bpy.app.handlers.load_post:bpy.app.handlers.load_post.append(loaded)
 for name, callback in _RENDER_HANDLERS:
  handlers = getattr(bpy.app.handlers, name)
  if callback not in handlers:handlers.append(callback)
# убираем подключения модуля при выключении аддона
def unregister():
 global _enabled,_handle,_display_pending,_token
 _enabled=False;_display_pending=False;_pointer_fields.clear();_token=object()
 gpu_ui.clear_batches()
 viewport_icons.clear(unload=True)
 _clear_update_feedback()
 _windows.clear();_modal_owner.clear();_hover.clear();_tooltip_shown.clear();_hit_regions.clear()
 if _handle is not None:bpy.types.SpaceView3D.draw_handler_remove(_handle,'WINDOW');_handle=None
 if bpy.app.timers.is_registered(tick):bpy.app.timers.unregister(tick)
 if loaded in bpy.app.handlers.load_post:bpy.app.handlers.load_post.remove(loaded)
 for name, callback in _RENDER_HANDLERS:
  handlers = getattr(bpy.app.handlers, name)
  if callback in handlers:handlers.remove(callback)
 for passes in _render_passes.values():_restore_drawing_passes(passes)
 _render_passes.clear()
 for name in ('project_damage_drawings_hidden','project_damage_drawing_opacity'):
  if hasattr(bpy.types.WindowManager,name):delattr(bpy.types.WindowManager,name)
 for cls in reversed(CLASSES):bpy.utils.unregister_class(cls)
