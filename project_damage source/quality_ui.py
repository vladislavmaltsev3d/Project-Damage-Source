# показываем качество в процентах, сохраняя внутренние единицы
"""Percentage controls preserve the native units of existing scenes and presets."""
import bpy
import sys
_registered=[]

def proxy_name(owner,name):
    proxy='pd_quality_'+name
    return proxy if proxy in owner.bl_rna.properties else name

# показываем понятный процент вместо внутренних единиц качества
def draw(layout,owner,name,**kwargs):
    proxy=proxy_name(owner,name)
    if proxy!=name:kwargs['text']='Rebar Resolution' if 'rebar' in name else 'Resolution'
    return layout.prop(owner,proxy,**kwargs)

# подключаем свойства и действия этого модуля к Blender
def register():
    unregister()
    classes={value for module in list(sys.modules.values()) if getattr(module,'__name__','').startswith(__package__+'.') for value in vars(module).values() if isinstance(value,type) and issubclass(value,bpy.types.PropertyGroup) and hasattr(value,'bl_rna')}
    for cls in classes:
        for prop in tuple(cls.bl_rna.properties):
            name=prop.identifier
            if prop.type not in {'FLOAT','INT'} or name.startswith('pd_quality_'):continue
            if prop.subtype=='PERCENTAGE' and name!='clay_resolution':continue
            inverse=False;base=1.
            if cls.__name__=='ProjectDamageProps':
                if name.endswith('_detail') and not any(s in name for s in ('noise','surface_detail','hole_detail')):
                    inverse=name in {'rust_detail','heat_detail','coating_zone_detail','metaltear_detail'}
                    base=prop.default if inverse or name=='peeling_detail' else 1.
                elif name.endswith('rebar_resolution'):base=1.
                elif name=='brick_back_resolution':base=1.
                elif name=='erosion_voxel_size':base=prop.default;inverse=True
                elif name=='clay_resolution':base=140.
                else:continue
            elif name in {'detail','resolution'} and cls.__name__ in {'PDPlankProps','ProjectDamageWeldSettings','ProjectDamageFoamSettings','ProjectDamageClothSettings','ProjectDamagePaperSettings'}:
                inverse=cls.__name__ not in {'PDPlankProps','ProjectDamageWeldSettings'}
                base=prop.default
            else:continue
            key='pd_quality_'+name
            minimum=max(.01,base*100/prop.hard_max) if inverse else max(0.,prop.hard_min/base*100)
            maximum=min(2000., (base*100/max(prop.hard_min,1e-8) if inverse else prop.hard_max/base*100))
            integer=prop.type=='INT'
            def accessors(n,b,inv,integer):
                def get(self):
                    value=getattr(self,n)
                    return b*100/max(value,1e-8) if inv else value/b*100
                def set_(self,value):
                    result=b*100/max(value,.01) if inv else value*b/100
                    setattr(self,n,round(result) if integer else result)
                return get,set_
            get,set_=accessors(name,base,inverse,integer)
            setattr(cls,key,bpy.props.FloatProperty(name='Resolution',subtype='PERCENTAGE',min=minimum,max=max(minimum,maximum),soft_max=min(200.,maximum),get=get,set=set_,description='Geometry sampling quality. Lower percentages use fewer polygons; 100% uses the reference resolution; higher values preserve finer geometry at a greater processing cost'))
            _registered.append((cls,key))

# убираем подключения модуля при выключении аддона
def unregister():
    for cls,key in reversed(_registered):
        if hasattr(cls,key):delattr(cls,key)
    _registered.clear()
