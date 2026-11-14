# кнопки справки открывают нужный раздел локальной документации
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline documentation links, scoped strictly to the Geometry asset drawer.

Edit PAGES/ALIASES to adjust destinations. HTML headings are indexed only when
opening a link; drawing the sidebar never reads HTML or requests a rebuild.
"""
from contextlib import contextmanager
from contextvars import ContextVar
from html.parser import HTMLParser
from pathlib import Path
from functools import lru_cache
import re
import unicodedata
from urllib.parse import quote
import webbrowser
import bpy

ROOT=Path(__file__).resolve().parent/'documentation'
_ACTIVE=ContextVar('project_damage_editorial_asset',default=None)
PAGES={
 'tools':'workflow/tools.html',
 'physics':'workflow/physics.html',
 'paper_generator':'deform/paper-generator.html',
 'fracture_surface':'fracture/surface-and-drawing.html',
 'surface':'worn/surface-damage.html',
 'edge':'worn/edge-damage.html',
 'corner':'worn/corner-damage.html',
 'cellf':'fracture/cell-fracture.html',
 'dbevel':'foam-metal/damaged-edge-bevel.html',
 'metaltear':'foam-metal/metal-tear-draw.html',
 'clay':'deform/clay.html',
 'peeling':'deform/peeling.html',
 'erosion':'deform/voxel-erosion.html',
 'plank_surface':'wood-lath/plank-surface.html',

 'fracture':'fracture/fracture.html','fracture_concrete':'fracture/fracture-concrete.html',
 'fracture_wood':'fracture/fracture-wood.html','fracture_glass':'fracture/fracture-glass.html',
 'fracture_radial':'fracture/fracture-radial.html','fracture_proximity_default':'fracture/fracture-proximity-default.html',
 'fracture_proximity_radial':'fracture/radial-fracture-proximity.html','fracture_proximity_glass':'fracture/glass-fracture-proximity.html',
 'buildup':'beads/beads.html','scratch':'beads/scratches-draw.html','fabric':'beads/fabric-classic.html',
 'fabric_solid':'beads/fabric-solidify.html','hit':'beads/concrete-hit.html','hit_through':'beads/concrete-hit-through.html',
 'plaster':'beads/plaster-displacement.html','wallpaper':'beads/wallpaper.html','bullet':'beads/bullet-impacts.html',
 'piles':'piles/corner-piles.html','deposits':'deposits/deposits.html',
 'ground':'ground/ground-cracks.html','fissure':'ground/ground-fissures.html',
 'grass':'nature/grass-scatter.html','gravel':'nature/gravel-scatter.html','ivy':'nature/ivy-generator.html','roots':'nature/roots-generator.html',
 'foam':'foam-metal/expanding-foam.html','rusty':'foam-metal/rusty-holes.html','weld':'foam-metal/welded-edges.html',
 'glass_hit':'glass/hit-glass.html','glass_drawn':'glass/glass-drawn.html',
 'cable_curve':'cables-cloth/cables-generator.html','cable_proximity':'cables-cloth/cables-generator.html',
 'cloth_hanging':'cables-cloth/cloth-sheets.html','cloth_curve':'cables-cloth/cloth-sheets.html','cloth_empties':'cables-cloth/cloth-sheets.html',
 'deform':'deform/impact-deform.html','concrete':'concrete/concrete-damage.html',
 'concrete_v2':'concrete/concrete-damage-v2.html','pits':'concrete/concrete-pitting.html','hang':'concrete/hanging-rebar-holes.html',
 'brick':'brick/bricks-generator.html','brick_v2':'brick/bricks-generator.html',
 'lath':'wood-lath/lath-damage.html','lath_v2':'wood-lath/laths-v2.html','lath_riven':'wood-lath/riven-lath.html','lath_framing':'wood-lath/stud-framing.html',
 'pipes':'pipes-cables/pipe-damage.html','cables':'pipes-cables/cable-damage.html',
 'boolean':'boolean/boolean-break.html',
}
for variant in ('rebar','central','bricks','planks','laths','framing','pipes','cables'):
 PAGES['boolean_'+variant]='boolean/boolean-break-'+variant+'.html'
PARENTS={'concrete_v2':'concrete','lath_v2':'lath','lath_riven':'lath','lath_framing':'lath','brick_v2':'brick'}
PARENTS.update({key:'boolean' for key in PAGES if key.startswith('boolean_')})
ALIASES={
 'organic placement experimental':('stable-volume-fill',), 'grid origin':('volume-grid-origin',),
 'fabric geometry':('fabric-geometry',), 'noise distribution':('fabric-noise',), 'fabric tears':('fabric-geometry',),
 'proximity objects':('damage-proximity',), 'wood laths':('wood-planks','boards','how-the-generator-works'),
 'basic settings':('general','basic-settings'), 'general settings':('general',),
 'damage zone':('damaged-zone','damaged-area'), 'exposed concrete':('exposed-concrete','spall-surface','spall-shape','damaged-zone'),
 'noisify':('noisify','noise','procedural-noise'), 'surface noise':('procedural-noise','surface-noise'),
 'bricks settings':('bricks','bricks-settings'), 'brick layer':('brick-layer','bricks'),
 'distribution and density':('distribution-and-density','density-and-distribution'),
 'placement logic':('placement-logic','layout'), 'fragments to separate objects':('separate-fragments',),
}

PAGES.update({key:'workflow/structural-surfaces.html' for key in ('structure_rebars','structure_pipes','structure_laths','structure_studs','structure_bricks')})
PAGES['gen_erosion']='workflow/structural-surfaces.html'

def normalize(text):
 text=unicodedata.normalize('NFKD',str(text)).encode('ascii','ignore').decode().lower()
 return re.sub(r'[^a-z0-9]+',' ',text).strip()


def enabled(context=None):
 context=context or bpy.context
 entry=context.preferences.addons.get(__package__)
 return bool(getattr(entry.preferences,'editorial_mode',True)) if entry else True


@contextmanager
def scope(key,source=None,active=True):
 if key=='boolean' and source is not None:
  from . import boolean_break
  variant=boolean_break.variant(source.project_damage).lower()
  if variant!='simple':key='boolean_'+variant
 token=_ACTIVE.set(key if active else None)
 try:yield
 finally:_ACTIVE.reset(token)


def button(row,title):
 key=_ACTIVE.get()
 if not key or not enabled():return
 icon=row.row(align=True);icon.scale_y=1.;icon.scale_x=1.
 op=icon.operator('project_damage.editorial_open',text='',icon='HELP')
 op.asset=key;op.section=title


class Headings(HTMLParser):
 def __init__(self):super().__init__();self.current=None;self.rows=[]
 def handle_starttag(self,tag,attrs):
  if tag in {'h1','h2','h3','h4'}:self.current=[tag,dict(attrs).get('id',''),'']
 def handle_data(self,text):
  if self.current:self.current[2]+=text
 def handle_endtag(self,tag):
  if self.current and self.current[0]==tag:self.rows.append(tuple(self.current));self.current=None


@lru_cache(maxsize=128)
def _headings(path,stamp):
 parser=Headings();parser.feed(Path(path).read_text(encoding='utf-8'));return parser.rows


def headings(path):return _headings(str(path),path.stat().st_mtime_ns)


# ищем нужную страницу и её заголовок для кнопки справки
def target(key,section):
 page=PAGES.get(key)
 if page is None:
  return ROOT/'editorial-missing.html',normalize(key).replace(' ','-')
 path=ROOT/page
 if not path.is_file():raise FileNotFoundError('Documentation file missing: '+str(path))
 title=normalize(section)
                                                                              
 if title in {normalize(t) for tag,_,t in headings(path) if tag=='h1'}:return path,''
 for asset in (key,PARENTS.get(key)):
  if not asset or asset not in PAGES:continue
  candidate=ROOT/PAGES[asset]
  rows=headings(candidate)
  for tag,anchor,text in rows:
   if tag in {'h2','h3'} and normalize(text)==title and anchor:return candidate,anchor
  aliases=ALIASES.get(title,())
  for anchor in aliases:
   if any(tag in {'h2','h3'} and a==anchor for tag,a,_ in rows):return candidate,anchor
                                                                           
 return path,''


# действие «Open Documentation»
class PROJECT_DAMAGE_OT_editorial_open(bpy.types.Operator):
 bl_idname='project_damage.editorial_open'
 bl_label='Open Documentation'
 bl_description='Open this asset or section in the bundled offline documentation'
 bl_options={'INTERNAL'}
 asset:bpy.props.StringProperty(options={'SKIP_SAVE'})
 section:bpy.props.StringProperty(options={'SKIP_SAVE'})
 @classmethod
 def description(cls,context,properties):return 'Open documentation: '+properties.section
 def execute(self,context):
  try:
   path,anchor=target(self.asset,self.section)
   if not path.is_file():raise FileNotFoundError(str(path))
   url=path.resolve().as_uri()+('#'+quote(anchor) if anchor else '')
   if not bpy.app.background:webbrowser.open(url,new=2)
   else:print('Project Damage documentation:',url)
  except (OSError,ValueError) as exc:
   self.report({'ERROR'},str(exc));return {'CANCELLED'}
  return {'FINISHED'}


# подключаем свойства и действия этого модуля к Blender
def register():bpy.utils.register_class(PROJECT_DAMAGE_OT_editorial_open)
# убираем подключения модуля при выключении аддона
def unregister():
 _headings.cache_clear()
 try:bpy.utils.unregister_class(PROJECT_DAMAGE_OT_editorial_open)
 except RuntimeError:pass

PAGES['structure_bricks']='brick/bricks-generator.html'

# старые кирпичные эффекты скрыты, справка ведёт к доступному генератору
PAGES['boolean_bricks']='brick/bricks-generator.html'
