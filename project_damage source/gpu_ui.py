# общие элементы рисуемого интерфейса и сохранённые данные для их показа
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small shared GPU drawing helpers. No scene changes from draw callbacks."""
import math
import blf
import gpu
from gpu_extras.batch import batch_for_shader

BROWSER_WINDOWS=set()

def scale(context):return max(.75,context.preferences.system.ui_scale)

_BATCHES={}
_BATCH_LIMIT=256

def cached_batch(key,build):
 """Reuse GPU batches between redraws; `build` returns (shader, batch)."""
 hit=_BATCHES.get(key)
 if hit is None:
  if len(_BATCHES)>=_BATCH_LIMIT:_BATCHES.clear()
  hit=_BATCHES[key]=build()
 return hit

def clear_batches():_BATCHES.clear()

def rect(x,y,w,h,color,radius=5):
 if w<=0 or h<=0:return
 def build():
  r=min(radius,w*.5,h*.5);points=[]
  for cx,cy,angle in ((x+w-r,y+h-r,0),(x+r,y+h-r,90),(x+r,y+r,180),(x+w-r,y+r,270)):
   for i in range(6):
    a=math.radians(angle+i*18);points.append((cx+r*math.cos(a),cy+r*math.sin(a)))
  shader=gpu.shader.from_builtin('UNIFORM_COLOR')
  return shader,batch_for_shader(shader,'TRI_FAN',{'pos':points})
 shader,batch=cached_batch(('rect',round(x,2),round(y,2),round(w,2),round(h,2),round(radius,2)),build)
 shader.bind();shader.uniform_float('color',color)
 batch.draw(shader)

def fit(text,width,size):
 blf.size(0,size)
 if blf.dimensions(0,text)[0]<=width:return text
 while text and blf.dimensions(0,text+'…')[0]>width:text=text[:-1]
 return text+'…' if text else ''

def text(value,x,y,size=13,color=(.94,.94,.96,1),width=None):
 blf.size(0,size);blf.color(0,*color)
 if width is not None:value=fit(value,width,size)
 blf.position(0,x,y,0);blf.draw(0,value)

def image(texture,x,y,w,h):
 iw,ih=texture.width,texture.height;ratio=min(w/max(iw,1),h/max(ih,1));tw=iw*ratio;th=ih*ratio
 x+=(w-tw)*.5;y+=(h-th)*.5
 shader=gpu.shader.from_builtin('IMAGE');shader.bind();shader.uniform_sampler('image',texture)
 batch_for_shader(shader,'TRI_FAN',{'pos':[(x,y),(x+tw,y),(x+tw,y+th),(x,y+th)],'texCoord':[(0,0),(1,0),(1,1),(0,1)]}).draw(shader)

def contains(box,x,y):return box[0]<=x<box[0]+box[2] and box[1]<=y<box[1]+box[3]

def sidebar_visible(area):
 if area.type!='VIEW_3D' or not area.spaces.active.show_region_ui:return False
 region=next((r for r in area.regions if r.type=='UI' and r.width>1),None)
 return bool(region and getattr(region,'active_panel_category','') in {'project_damage','⚒ Project Damage'})

def helper_origin(area,region,width,height,padding):
 """Region rectangles are window coordinates; shelf regions may overlap WINDOW."""
 x=region.x+padding;y=region.y+padding;right=region.x+region.width-padding
 for r in area.regions:
  if r.width<=1 or r.height<=1:continue
  if r.type=='UI':right=min(right,r.x-padding)
  if r.type=='TOOLS' and r.x<=x<r.x+r.width:x=r.x+r.width+padding
 for _ in range(len(area.regions)+1):
  shifted=False
  for r in area.regions:
   if r.type not in {'ASSET_SHELF','ASSET_SHELF_HEADER','HEADER','TOOL_HEADER','HUD'} or r.width<=1 or r.height<=1:continue
   if r.y>region.y+region.height*.5:continue
   if x<r.x+r.width and x+width>r.x and y<r.y+r.height and y+height>r.y:
    y=r.y+r.height+padding;shifted=True
  if not shifted:break
 return x,y,max(0,right-x)
