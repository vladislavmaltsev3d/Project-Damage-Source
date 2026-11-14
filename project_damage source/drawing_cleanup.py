# после Apply удаляем только рисунки, принадлежащие этому эффекту
"""Remove only this applied setup's private drawing helpers."""
import bpy


def after_apply(source, outputs):
    drawings=[ob for ob in bpy.data.objects if ob.type in {'GREASEPENCIL','GPENCIL'} and ob.get('project_damage_paint_source')==source]
    if not drawings:return
    users=bpy.data.user_map(subset=set(drawings))
    allowed={source,*outputs}
    for drawing in drawings:
        foreign={user for user in users.get(drawing,set()) if isinstance(user,bpy.types.Object) and user not in allowed and user!=drawing}
        if foreign:continue
        data=drawing.data
        bpy.data.objects.remove(drawing,do_unlink=True)
        if data.users==0:
            collection=getattr(bpy.data,'grease_pencils_v3',None)
            if collection is None:collection=getattr(bpy.data,'grease_pencils',None)
            if collection is not None and data.name in collection:collection.remove(data)
