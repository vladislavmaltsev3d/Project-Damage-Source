# защищаем включение и выключение аддона от неполной регистрации
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover package-owned RNA after an interrupted enable without losing data."""
from contextlib import contextmanager
import sys
import bpy


PACKAGE=__package__


def _owned(cls):
    return getattr(cls,'__module__','').startswith(PACKAGE+'.')


def _actual(cls):
    for base in (bpy.types.PropertyGroup,bpy.types.Operator,bpy.types.Panel,
                 bpy.types.UIList,bpy.types.Menu,bpy.types.GizmoGroup,bpy.types.Gizmo,
                 bpy.types.AddonPreferences):
        if issubclass(cls,base):
            rna=cls.__dict__.get('bl_rna')
            for name in (getattr(rna,'identifier',''),cls.__name__,getattr(cls,'bl_idname','')):
                if name:
                    actual=base.bl_rna_get_subclass_py(name,None)
                    if actual is not None:return actual
            break
    return None


@contextmanager
def guard():
    """Scope wrappers to this enable/disable; leave other addons untouched.

    Property groups are adopted so existing Object/Scene data stays bound.
    UI classes/operators left by older Python definitions are replaced.
    On failure, also remove classes created in the module that raised.
    """
    original_register=bpy.utils.register_class
    original_unregister=bpy.utils.unregister_class
    created=[];adopted={}

    def register(cls):
        if not _owned(cls):return original_register(cls)
        actual=_actual(cls)
        if actual is cls:return None
        if actual is not None:
            if actual.__module__!=cls.__module__:
                raise RuntimeError('RNA name belongs to another addon: '+cls.__name__)
            if issubclass(cls,bpy.types.PropertyGroup):
                adopted[cls]=actual
                setattr(sys.modules[cls.__module__],cls.__name__,actual)
                return None
            original_unregister(actual)
        for value in cls.__dict__.get('__annotations__',{}).values():
            keywords=getattr(value,'keywords',None)
            if keywords is not None and keywords.get('type') in adopted:
                keywords['type']=adopted[keywords['type']]
        result=original_register(cls);created.append(cls)
        return result

    def unregister(cls):
        if not _owned(cls):return original_unregister(cls)
        actual=_actual(cls)
        if actual is None:return None
        if actual.__module__!=cls.__module__:
            raise RuntimeError('RNA name belongs to another addon: '+cls.__name__)
        return original_unregister(actual)

    bpy.utils.register_class=register
    bpy.utils.unregister_class=unregister
    try:yield
    except BaseException:
        for cls in reversed(created):
            if _actual(cls) is cls:
                try:original_unregister(cls)
                except RuntimeError:pass
        raise
    finally:
        bpy.utils.register_class=original_register
        bpy.utils.unregister_class=original_unregister
