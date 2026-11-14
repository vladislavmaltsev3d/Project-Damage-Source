# оставляем результаты в нужных коллекциях, отдельно от скрытых источников
import bpy

SOURCE_COLLECTION = 'project_damage_source_collection'
OUTPUT_COLLECTION = 'project_damage_output_collection'


def source_collections():
    roots=[c for c in bpy.data.collections
           if c.get(SOURCE_COLLECTION) or c.name=='Project Damage · Sources']
    return {c for root in roots for c in (root,*root.children_recursive)}


def destination(context,source):
    from . import build
    excluded=source_collections()
    scene_collections={context.scene.collection,*context.scene.collection.children_recursive}
    def valid(coll):
        return isinstance(coll,bpy.types.Collection) and coll in scene_collections and coll not in excluded and coll.is_editable
    pending=source.get('project_damage_preparation_target')
    for obj in (pending,build.result_of(source)):
        if isinstance(obj,bpy.types.Object):
            for coll in obj.users_collection:
                if valid(coll):return coll
    stored=source.get(OUTPUT_COLLECTION)
    if valid(stored):return stored
    for coll in source.users_collection:
        if valid(coll):return coll
    return context.scene.collection


def link_object(context,source,obj):
    excluded=source_collections()
    scene_collections={context.scene.collection,*context.scene.collection.children_recursive}
    if not any(c in scene_collections and c not in excluded for c in obj.users_collection):
        target=destination(context,source)
        if obj.name not in target.objects:target.objects.link(obj)
    for coll in tuple(obj.users_collection):
        if coll in excluded:coll.objects.unlink(obj)


def link_collection(context,source,collection):
    excluded=source_collections()
    scene_collections={context.scene.collection,*context.scene.collection.children_recursive}
    parents=[c for c in scene_collections if collection.name in c.children]
    if not any(c not in excluded for c in parents):
        target=destination(context,source)
        if target==collection or target in collection.children_recursive:target=context.scene.collection
        if collection.name not in target.children:target.children.link(collection)
    for parent in parents:
        if parent in excluded:parent.children.unlink(collection)


def repair(context,source):
    from . import ownership
    uid=source.get('project_damage_uid')
    if not uid:return
    excluded=source_collections()
    for coll in tuple(excluded):
        if any(key.startswith('project_damage') and key.endswith('_collection') and coll.get(key)==uid for key in coll.keys()):
            link_collection(context,source,coll)
    excluded=source_collections()
    for obj in {obj for coll in excluded for obj in coll.objects}:
        if obj!=source and ownership.source_of(obj)==source:
            link_object(context,source,obj)
