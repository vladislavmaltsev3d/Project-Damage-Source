# считаем тяжёлые задачи в фоне и возвращаем готовый результат в сцену
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cooperative Blender-side continuations, with one low priority worker.

Only job transport (plain independent Python/numpy data) touches the I/O
thread. RNA and generator continuations always run on Blender's main thread.
"""
import atexit
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import os
from pathlib import Path
import pickle
import shutil
import signal
import subprocess
import tempfile
import time
import traceback
import bpy
from . import background_wire

_jobs = {}
_retired = []
_errors = {}
_notices = {}
_io = None
_current = None
_sync_depth = 0
_resuming = False
_quiet_after=0.
_gestures={}
_idle_worker=None
IDLE_WORKER_SECONDS=90.
STALE_FOLDER_SECONDS=86400.
from .exact_cache import Results
_exact_results=Results()
_noted=set()

def _note(where,exc):
    """A failed cleanup step must not stop the job; report it once per place."""
    if where not in _noted:
        _noted.add(where);print('[ProjectDamage] background %s: %r'%(where,exc))

def user_input(window,event):
    """Observe (never consume) user input; no RNA scene reads on mouse moves."""
    global _quiet_after
    if not _jobs:return
    kind=event.type
    if kind=='TIMER':return
    ptr=window.as_pointer();now=time.monotonic()
    if kind in {'LEFTMOUSE','MIDDLEMOUSE','RIGHTMOUSE'}:
        if event.value=='PRESS':_gestures[ptr]=now+3.
        elif event.value=='RELEASE':_gestures.pop(ptr,None)
    elif kind=='WINDOW_DEACTIVATE':
        _gestures.pop(ptr,None);return
    elif kind in {'MOUSEMOVE','INBETWEEN_MOUSEMOVE'}:
        if _gestures.get(ptr,0.)<now:return
        _gestures[ptr]=now+3.
    elif kind not in {'WHEELUPMOUSE','WHEELDOWNMOUSE','TRACKPADPAN','TRACKPADZOOM','NDOF_MOTION'}:
        return
    _quiet_after=now+.18

def interacting(scene):
    if time.monotonic()<_quiet_after:return True
    if bpy.app.background:return False
    for win in bpy.context.window_manager.windows:
        if win.scene!=scene:continue
                                                                          
        for op in getattr(win,'modal_operators',()):
            name=op.bl_idname
            if name.startswith(('TRANSFORM_OT_','transform.')) or name in {'VIEW3D_OT_rotate','VIEW3D_OT_move','VIEW3D_OT_zoom','view3d.rotate','view3d.move','view3d.zoom'}:
                return True
        for area in win.screen.areas:
            if area.type=='VIEW_3D' and getattr(area.spaces.active.region_3d,'is_navigating',False):return True
    return False

class Deferred(Exception):
    pass

@contextmanager
def synchronous():
    global _sync_depth
    _sync_depth += 1
    try: yield
    finally: _sync_depth -= 1

def enabled(source):
    return bool(source is not None and source.project_damage.background_compute)

def async_for(source):
    return enabled(source) and not _sync_depth

def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D': area.tag_redraw()

class Request:
    def __init__(self, label, func, args, kw, native=None):
        self.label, self.func, self.args, self.kw, self.native = label, func, args, kw, native
    def packet(self):
        from . import incremental
        flags={'_pd_intermediate_cache':incremental.cache_enabled(),
               '_pd_owner':_current['key'][0] if _current is not None else None}
        if self.native:
            kw = {k:v for k,v in self.kw.items() if k!='cache'}
            kw.update(flags)
            if self.func.__name__=='build_worn':kw['_pd_native_cache']=self.kw.get('cache') is not None
            return ('native', self.native.__file__, self.func.__name__, background_wire.encode(self.args,self.native),background_wire.encode(kw,self.native))
        return ('task',self.func.__module__, self.func.__name__, self.args,dict(self.kw,**flags))

def task(label, func, *args, **kw):
    return (yield Request(label,func,args,kw))

def native(core, method, *args, **kw):
    return (yield Request('Calculating geometry',getattr(core,method),args,kw,core))

# тот же расчёт можно выполнить сразу, без фоновой очереди
def immediate(iterator):
    try:
        req=next(iterator)
        while True:
            try:value=evaluate(req) if req is not None else None
            except Exception as exc:req=iterator.throw(exc)
            else:req=iterator.send(value)
    except StopIteration as done:return done.value

def evaluate(req):
    from . import incremental
    allowed=req.func.__module__.rsplit('.',1)[-1] in {'fabric_prep','mesh_prep','mesh_prep_fast','cell_cuts','damage_shading','input_prep','mesh_io','result_transfer','inner_cutters'}
    cached=incremental.cache_enabled() and (allowed or req.native is not None)
    key=None
    if cached:
        if req.native is not None:
            args=background_wire.encode(req.args,req.native);kw=background_wire.encode({k:v for k,v in req.kw.items() if k!='cache'},req.native)
        else:args,kw=req.args,req.kw
        key=_exact_results.key(req.func.__module__,req.func.__name__,args,kw)
        found,result=_exact_results.get(key)
        if found:return result
    result=req.func(*req.args,**req.kw)
    if cached:_exact_results.put(key,result)
    return result

def clear_intermediates():
    global _idle_worker
    _exact_results.clear()
    if _idle_worker is not None:
        old=_idle_worker;_idle_worker=None
        old['retain_worker']=False;_dispose(old)
        _ensure_timer()


def stage_mesh(mesh):
    if _current is not None: _current['staged'].append(mesh)
    return mesh

def valid_id(ob):
    try: return ob is not None and bool(ob.as_pointer())
    except ReferenceError: return False

def _put(folder, packet):
    with (folder/'next.tmp').open('wb') as f: pickle.dump(packet,f,protocol=5)
    os.replace(folder/'next.tmp',folder/'next')
def _get(folder):
    with (folder/'reply').open('rb') as f: value=pickle.load(f)
    (folder/'reply').unlink()
    return value

def _pool():
    global _io
    if _io is None: _io=ThreadPoolExecutor(max_workers=1,thread_name_prefix='PD file transport')
    return _io

def cpu_budget():
    entry=bpy.context.preferences.addons.get(__package__)
    return max(10,min(100,int(getattr(entry.preferences,'background_cpu_budget',75)))) if entry else 75

def _resume(j):
    proc=j.get('process')
    if j.pop('cpu_paused',False) and proc and proc.poll() is None:
        try:os.kill(proc.pid,signal.SIGCONT)
        except (OSError,AttributeError) as exc:_note('resume worker',exc)

def _budget_tick():
    if not _jobs:return None
    budget=cpu_budget()
    for j in list(_jobs.values()):
        proc=j.get('process')
        if not proc or proc.poll() is not None:continue
        if os.name=='posix':
                                                                          
            paused=budget<100 and (time.monotonic()-j.get('cpu_epoch',0.))%.2 >= .2*budget/100.
            if paused!=j.get('cpu_paused',False):
                try:os.kill(proc.pid,signal.SIGSTOP if paused else signal.SIGCONT);j['cpu_paused']=paused
                except OSError as exc:_note('set worker budget',exc)
        elif os.name=='nt':
            _windows_cpu_limit(j,budget)
    return .025

def _windows_cpu_limit(j,budget):
    if j.get('cpu_budget')==budget:return
    import ctypes
    from ctypes import wintypes
    class Rate(ctypes.Structure):_fields_=[('flags',wintypes.DWORD),('rate',wintypes.DWORD)]
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateJobObjectW.restype=wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes=[wintypes.HANDLE,ctypes.c_int,ctypes.c_void_p,wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes=[wintypes.HANDLE,wintypes.HANDLE]
    handle=j.get('cpu_job')
    if handle is None:
        handle=kernel.CreateJobObjectW(None,None)
        if not handle:return
        if not kernel.AssignProcessToJobObject(handle,wintypes.HANDLE(int(j['process']._handle))):kernel.CloseHandle(wintypes.HANDLE(handle));return
        j['cpu_job']=handle
        try:
            # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the worker cannot outlive a crashed Blender.
            class Basic(ctypes.Structure):_fields_=[('a',ctypes.c_int64),('b',ctypes.c_int64),('flags',wintypes.DWORD),('c',ctypes.c_size_t),('d',ctypes.c_size_t),('e',wintypes.DWORD),('f',ctypes.c_size_t),('g',wintypes.DWORD),('h',wintypes.DWORD)]
            class Extended(ctypes.Structure):_fields_=[('basic',Basic),('io',ctypes.c_uint64*6),('m1',ctypes.c_size_t),('m2',ctypes.c_size_t),('m3',ctypes.c_size_t),('m4',ctypes.c_size_t)]
            info=Extended();info.basic.flags=0x2000
            kernel.SetInformationJobObject(handle,9,ctypes.byref(info),ctypes.sizeof(info))
        except Exception as exc:_note('set worker job limit',exc)
    rate=Rate(5,max(1,budget*100//max(1,os.cpu_count() or 1)))
    if kernel.SetInformationJobObject(handle,15,ctypes.byref(rate),ctypes.sizeof(rate)):j['cpu_budget']=budget

def _start_worker(j):
    global _idle_worker
    from . import incremental
    if _idle_worker is not None:
        previous=_idle_worker;_idle_worker=None
        if incremental.cache_enabled() and j['key'][1]!='python_operation' and previous['process'].poll() is None:
            for name in ('process','folder','cpu_job','cpu_budget'):
                if name in previous:j[name]=previous[name]
            j['cpu_epoch']=time.monotonic()
            if not bpy.app.timers.is_registered(_budget_tick):bpy.app.timers.register(_budget_tick,first_interval=.025)
            return
        previous['retain_worker']=False;_dispose(previous)
    folder=Path(tempfile.mkdtemp(prefix='project_damage_job_'));j['folder']=folder
    env=dict(os.environ,PD_PRIVATE_WORKER='1',OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1',
             VECLIB_MAXIMUM_THREADS='1',MKL_NUM_THREADS='1',NUMEXPR_NUM_THREADS='1',BLIS_NUM_THREADS='1')
    cmd=[bpy.app.binary_path,'--background','--factory-startup','--disable-autoexec','-t','1',
         '--python-exit-code','7','--python',str(Path(__file__).with_name('background_worker.py')),'--',str(folder),str(os.getpid())]
    if os.name=='posix' and Path('/usr/bin/nice').exists(): cmd=['/usr/bin/nice','-n','12',*cmd]
    flags=getattr(subprocess,'BELOW_NORMAL_PRIORITY_CLASS',0) if os.name=='nt' else 0
    with (folder/'worker.log').open('wb') as log:
        j['process']=subprocess.Popen(cmd,stdout=log,stderr=log,env=env,creationflags=flags)
    try:(folder/'owner.pid').write_text('%d %d'%(os.getpid(),j['process'].pid))
    except OSError as exc:_note('write worker marker',exc)
    j['cpu_epoch']=time.monotonic()
    if not bpy.app.timers.is_registered(_budget_tick):bpy.app.timers.register(_budget_tick,first_interval=.025)

def _dispose(j):
    global _idle_worker
    _resume(j)
    gen=j.pop('iterator',None)
    if gen is not None:
        try: gen.close()
        except (ReferenceError,RuntimeError) as exc:_note('close continuation',exc)
    for me in j['staged']:
        if valid_id(me) and not me.users: bpy.data.meshes.remove(me)
    j['staged'].clear()
    proc=j.get('process')
    if j.get('retain_worker') and proc is not None and proc.poll() is None:
        if _idle_worker is not None:
            old=_idle_worker;_idle_worker=None;old['retain_worker']=False;_dispose(old)
        _idle_worker=j;j['idle_since']=time.monotonic()
        if not bpy.app.timers.is_registered(_idle_check):bpy.app.timers.register(_idle_check,first_interval=5.)
        return
    if proc is not None and proc.poll() is None:
        try: proc.terminate()
        except OSError as exc:_note('terminate worker',exc)
    if j.get('folder') is not None:
        j['retired_at']=time.monotonic();_retired.append(j)

def _idle_check():
    """A retained worker is a whole Blender process: release it after a pause in work."""
    global _idle_worker
    j=_idle_worker
    if j is None:return None
    proc=j.get('process')
    if proc is not None and proc.poll() is None and time.monotonic()-j.get('idle_since',0.)<IDLE_WORKER_SECONDS:return 5.
    _idle_worker=None;j['retain_worker']=False;_dispose(j);_ensure_timer()
    return None

def _pid_alive(pid):
    try:os.kill(pid,0)
    except ProcessLookupError:return False
    except OSError:return True
    return True

def _sweep_stale():
    """Remove job folders (and stopped workers) left by a Blender that crashed."""
    try:entries=list(os.scandir(tempfile.gettempdir()))
    except OSError:return
    now=time.time();own={str(j['folder']) for j in list(_jobs.values())+_retired if j.get('folder')}
    if _idle_worker is not None and _idle_worker.get('folder'):own.add(str(_idle_worker['folder']))
    for entry in entries:
        try:
            if not entry.name.startswith(('project_damage_job_','project_damage_scene_')) or not entry.is_dir(follow_symlinks=False) or entry.path in own:continue
            old=now-entry.stat(follow_symlinks=False).st_mtime>STALE_FOLDER_SECONDS
            marker=Path(entry.path)/'owner.pid'
            if os.name=='posix' and marker.is_file():
                parent,worker=(int(v) for v in marker.read_text().split()[:2])
                if _pid_alive(parent):continue
                if _pid_alive(worker):
                    # Only a process that still runs our worker script; a pid may have been reused.
                    command=subprocess.run(['/bin/ps','-o','command=','-p',str(worker)],capture_output=True,text=True,timeout=5).stdout
                    if 'background_worker.py' in command and entry.name in command:
                        os.kill(worker,signal.SIGKILL)
            elif not old:continue
            shutil.rmtree(entry.path,ignore_errors=True)
        except (OSError,ValueError,subprocess.SubprocessError) as exc:_note('remove stale job folder',exc)

def _reap():
    for j in _retired[:]:
        p=j.get('process');f=j.get('io')
        if p is not None and p.poll() is None:
            if time.monotonic()-j['retired_at']>1:
                try:p.kill()
                except OSError as exc:_note('kill retired worker',exc)
            continue
        if f is not None and not f.done(): continue
        if j.get('cpu_job'):
            import ctypes
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(j.pop('cpu_job')))
        shutil.rmtree(j['folder'],ignore_errors=True);_retired.remove(j)

# отменяем задачу, результат которой уже не нужен
def cancel(source=None,method=None):
    uid=source.session_uid if source is not None else None
    if source is None:clear_intermediates()
    changed=False
    for key,j in list(_jobs.items()):
        if (uid is None or key[0]==uid) and (method is None or key[1]==method):
            _jobs.pop(key);_dispose(j);changed=True
    for key in list(_notices):
        if (uid is None or key[0]==uid) and (method is None or key[1]==method):
            _notices.pop(key);changed=True
    if uid is None:_errors.clear()
    else:_errors.pop(uid,None)
    if changed:_redraw()
    if _retired:_ensure_timer()

def status(source):
    notices=[j for key,j in _jobs.items() if key[0]==source.session_uid]
    return notices[0]['label'] if notices else _errors.get(source.session_uid,'')

def notice(scene):
                                                                                
    running=next((j for j in _jobs.values() if j['scene']==scene),None)
    if running:return running['key'],'RUNNING',running['label']
    for key,(owner,sc,state,msg) in reversed(_notices.items()):
        if sc==scene and valid_id(owner):return key,state,msg
    return None

def notice_source(key):
    job=_jobs.get(key)
    return job['owner'] if job else _notices.get(key,(None,))[0]

def dismiss(key):
    _notices.pop(key,None);_redraw()

def setting_changed(self,context):
    cancel(self.id_data)

def _ensure_timer():
    if not bpy.app.timers.is_registered(_tick):bpy.app.timers.register(_tick,first_interval=.025)

def queue(source, method, factory, *, guard=None):
    key=(source.session_uid,method)
    cancel(source,method)
    _jobs[key]={'key':key,'owner':source,'scene':bpy.context.scene,'layer':bpy.context.view_layer.name,
                'factory':factory,'guard':guard,'iterator':None,'phase':'step','value':None,
                'process':None,'folder':None,'io':None,'staged':[],'label':'Operation in progress: queued',
                'result':None,'source_mesh':source.data,'last_activity':time.monotonic()}
    _ensure_timer();_redraw()

def invalidate(source):
                                                                             
                                                                             
                                                                    
    key=(source.session_uid,'build_worn')
    j=_jobs.get(key)
    if j and j['iterator'] is not None:
        queue(source,'build_worn',j['factory'])

def pending(source,method):
    return (source.session_uid,method) in _jobs

def _finish(j, error=None):
    key=j['key'];_jobs.pop(key,None)
    if error:_errors[key[0]]=str(error);print('Project Damage:',error,flush=True)
    for previous,n in list(_notices.items()):
        if previous[0]==key[0] and n[2]=='READY':_notices.pop(previous)
    _notices.pop(key,None)
    _notices[key]=(j['owner'],j['scene'],'ERROR' if error else 'READY',str(error) if error else 'Operation complete')
    while len(_notices)>32:_notices.pop(next(iter(_notices)))
    from . import incremental
    j['retain_worker']=not error and j['key'][1]!='python_operation' and incremental.cache_enabled()
    _dispose(j);_redraw()
                                                                       
                                                                         
    if not error and not bpy.app.background and not any(k[0]==key[0] for k in _jobs):
        try:
            if bpy.context.preferences.edit.use_global_undo and bpy.ops.ed.undo_push.poll():
                bpy.ops.ed.undo_push(message='Project Damage: Background Result')
        except RuntimeError as exc:_note('save undo step',exc)

def _tick():
    global _current,_resuming
    from . import live,build
    _reap()
    if live._HISTORY_RESTORING:
        cancel();return .1 if _retired else None
    if live._APPLYING:return .05
    if not _jobs:
        _gestures.clear();return .1 if _retired else None
    if _retired:return .04
                                                                            
    j=next(iter(_jobs.values()));key=j['key'];source=j['owner'];scene=j['scene']
    try:
        if not valid_id(source) or not valid_id(scene) or not enabled(source) or source.data!=j['source_mesh'] or source.name not in scene.objects:
            _jobs.pop(key);_dispose(j);_redraw();return .02
        if j['guard'] is not None and not j['guard']():
            _jobs.pop(key);_dispose(j);_redraw();return .02
        layer=scene.view_layers.get(j['layer'])
        if layer is None: raise RuntimeError('The original view layer no longer exists')
        proc=j['process'];phase=j['phase']
        if proc and proc.poll() is not None:
            log=(j['folder']/'worker.log')
            with log.open('rb') as f:
                f.seek(max(0,log.stat().st_size-2500));detail=f.read().decode(errors='replace')
            raise RuntimeError('Background process stopped: '+detail[-700:])
        if phase=='write':
            if not j['io'].done():return .03
            j['io'].result();j['io']=None;j['phase']='wait';return .02
        if phase=='wait':
            if not (j['folder']/'reply').is_file():return .04
            j['io']=_pool().submit(_get,j['folder']);j['phase']='read';return .01
        if phase=='read':
            if not j['io'].done():return .025
            ok,j['value']=j['io'].result();j['io']=None
            if not ok:raise RuntimeError(j['value'])
            j['phase']='step'
                                                                                              
        if interacting(scene):return .045
        target=build.result_of(source)
        if live._busy(source):
            j['label']='Waiting for drawing or editing to finish';return .15
        if source.mode!='OBJECT' or (target and target.mode!='OBJECT'):
            j['label']='Waiting for Object Mode';return .15
        if j['iterator'] is not None and target!=j['result']:
            raise RuntimeError('The preview object was replaced during calculation')
        if j['iterator'] is None:
            j['iterator']=j['factory']();j['result']=target
        _current=j;_resuming=True;live._APPLYING=True
        try:
            with bpy.context.temp_override(scene=scene,view_layer=layer):
                req=j['iterator'].send(j.pop('value',None))
        except StopIteration as done:
            result=done.value;error=None
            if key[1]=='build_worn':
                obj,reports=result
                error=next((msg for flags,msg in reports if 'ERROR' in flags),None)
                if obj is None and not error:error='Background rebuild did not produce a mesh'
            elif result=={'CANCELLED'}:error='Generator did not produce a result'
            _finish(j,error);return .02
        finally:
            live._APPLYING=False;_current=None;_resuming=False
        if req is not None:
            if j['process'] is None:_start_worker(j)
            j['label']=req.label+'…'
            j['io']=_pool().submit(_put,j['folder'],req.packet());j['phase']='write'
        return .012
    except Exception as exc:
        traceback.print_exc();_finish(j,exc);return .04

HEAVY_MESH_POLYGONS = 100_000
BRICK_MESH_POLYGONS = 150_000
HEAVY_EFFECTS = {
    'boolean', 'cellf', 'erosion', 'concrete', 'concrete_v2', 'brick', 'brick_v2',
    'lath', 'lath_v2', 'lath_riven', 'lath_framing', 'fabric', 'fabric_solid',
    'glass_hit', 'glass_drawn', 'metaltear', 'pipes', 'cables', 'wallpaper', 'ground', 'fissure',
}


def confirm_effect(context, source, key, action, variant=''):
    """Show the preflight before an interactive add or refresh, once per click."""
    if source is None:
        return False
    polygons = len(source.data.polygons)
    threshold = BRICK_MESH_POLYGONS if key in {'brick', 'brick_v2'} else HEAVY_MESH_POLYGONS
    always_confirm = key == 'peeling' or (action == 'ADD' and key in {'pits', 'plaster'})
    if not always_confirm and (polygons <= threshold or
                               (key not in HEAVY_EFFECTS and not key.startswith('fracture'))):
        return False

    from . import sidebar
    title = sidebar.CATALOG[key][1]
    target_name, target_uid = source.name, str(source.session_uid)

    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'EXEC_DEFAULT'
        if polygons > threshold:
            layout.label(text=f'This mesh has {polygons:,} polygons.')
        layout.label(text=f'{title} may take some time to calculate.')
        layout.label(text='We recommend Background Calculation.')
        layout.label(text='You can keep working during background steps.')
        layout.separator()
        for choice, label, icon in (
            ('BACKGROUND', 'Enable Background Calculation and Continue', 'TIME'),
            ('FOREGROUND', 'Continue without Background Calculation', 'PLAY'),
            ('CANCEL', 'Cancel', 'CANCEL'),
        ):
            op = layout.operator('project_damage.continue_effect', text=label, icon=icon)
            op.effect, op.action, op.variant = key, action, variant
            op.target_name, op.target_uid, op.choice = target_name, target_uid, choice

    context.window_manager.popup_menu(draw, title='Continue with ' + title, icon='INFO')
    return True


# действие «Continue Project Damage Effect»
class PROJECT_DAMAGE_OT_continue_effect(bpy.types.Operator):
    bl_idname = 'project_damage.continue_effect'
    bl_label = 'Continue Project Damage Effect'
    bl_options = {'INTERNAL', 'UNDO'}
    effect: bpy.props.StringProperty(options={'SKIP_SAVE'})
    action: bpy.props.StringProperty(options={'SKIP_SAVE'})
    variant: bpy.props.StringProperty(options={'SKIP_SAVE'})
    target_name: bpy.props.StringProperty(options={'SKIP_SAVE'})
    target_uid: bpy.props.StringProperty(options={'SKIP_SAVE'})
    choice: bpy.props.EnumProperty(items=(
        ('BACKGROUND', 'Background Calculation', ''),
        ('FOREGROUND', 'Foreground Calculation', ''),
        ('CANCEL', 'Cancel', ''),
    ), default='CANCEL', options={'SKIP_SAVE'})

    def execute(self, context):
        from . import build, live, sidebar
        if self.choice == 'CANCEL':
            return {'CANCELLED'}
        target = context.view_layer.objects.get(self.target_name)
        if target is None or str(target.session_uid) != self.target_uid or context.mode != 'OBJECT':
            return {'CANCELLED'}
        if self.effect not in sidebar.CATALOG or self.action not in {'ADD', 'APPLY'}:
            return {'CANCELLED'}
        with context.temp_override(project_damage_import_source=target):
            if not sidebar.source(context):
                return {'CANCELLED'}
            target.project_damage.background_compute = self.choice == 'BACKGROUND'
            if self.variant:
                status = bpy.ops.project_damage.boolean_variant_add(variant=self.variant, defer_rebuild=True)
            else:
                status = bpy.ops.project_damage.effect_action(effect=self.effect, action=self.action)
            if status != {'FINISHED'} or self.action == 'APPLY':
                return status
            live._dirty.pop(target.get('project_damage_uid'), None)
            result, reports = build.rebuild_object(context, target)
            for flags, message in reports:
                self.report(flags, message)
            # Keep the new settings undoable even if generation was unable to finish.
            return {'FINISHED'}


# показываем настройки выбранного эффекта
def draw_settings(layout, context, source):
    props = source.project_damage
    entry = context.preferences.addons.get(__package__)
    box = layout.box()
    box.use_property_split = False
    box.label(text='Background Calculation', icon='TIME')
    row = box.row()
    row.prop(props, 'background_compute', text='Background Calculation', toggle=True, icon='TIME')
    if entry is not None:
        row = box.row()
        row.enabled = props.background_compute
        row.prop(entry.preferences, 'background_cpu_budget', slider=True)
    message = status(source)
    if message:
        box.label(text=message, icon='INFO')
        box.operator('project_damage.cancel_background', text='Cancel', icon='CANCEL')

    box = layout.box()
    box.use_property_split = False
    for field, label, icon in (
        ('perf_mesh_preparation', 'Mesh Preparation', 'MESH_DATA'),
        ('perf_ngon_restore', 'N-gon Restore', 'FACESEL'),
        ('perf_prep_cache', 'Mesh Prep Cache', 'DISK_DRIVE'),
    ):
        box.row().prop(props, field, text=label, toggle=True, icon=icon)
    if entry is not None:
        prefs = entry.preferences
        box.row().prop(prefs, 'cache_intermediates', text='Intermediate Cache', toggle=True, icon='DISK_DRIVE')
        row = box.row()
        row.enabled = prefs.cache_intermediates
        row.prop(prefs, 'incremental_updates', text='Only Changed Generators', toggle=True, icon='MOD_BUILD')
    box.row().prop(context.window_manager, 'project_damage_core_profiler', text='Core Profiler', toggle=True, icon='TIME')
    if context.window_manager.project_damage_core_profiler:
        box.label(text='Timings appear in the Blender console.', icon='INFO')


class PROJECT_DAMAGE_OT_cancel_background(bpy.types.Operator):
    bl_idname='project_damage.cancel_background';bl_label='Cancel Background Calculation';bl_options={'INTERNAL'}
    def execute(self,context):
        from . import ownership,live,ivy,roots
        ob=ownership.context_source(context)
        if ob is not None:
            cancel(ob);live._dirty.pop(ob.get('project_damage_uid'),None)
            ivy._pending.pop(ob.name,None);roots._pending.pop(ob.name,None)
        return {'FINISHED'}

def _shutdown():
    clear_intermediates()
    if bpy.app.timers.is_registered(_budget_tick):bpy.app.timers.unregister(_budget_tick)
    if bpy.app.timers.is_registered(_idle_check):bpy.app.timers.unregister(_idle_check)
    global _io
    for j in list(_jobs.values())+_retired:
        p=j.get('process')
        if p and p.poll() is None:
            try:p.kill();p.wait(timeout=2)
            except (OSError,subprocess.TimeoutExpired) as exc:_note('stop worker on shutdown',exc)
    if _io is not None:_io.shutdown(wait=True,cancel_futures=True);_io=None
    for j in list(_jobs.values())+_retired:
        if j.get('folder'):shutil.rmtree(j['folder'],ignore_errors=True)
    _jobs.clear();_retired.clear();_notices.clear();_errors.clear()

# подключаем свойства и действия этого модуля к Blender
def register():
    bpy.utils.register_class(PROJECT_DAMAGE_OT_continue_effect)
    bpy.utils.register_class(PROJECT_DAMAGE_OT_cancel_background);atexit.register(_shutdown)
    if os.environ.get('PD_PRIVATE_WORKER')!='1':_sweep_stale()
# убираем подключения модуля при выключении аддона
def unregister():
    cancel();_shutdown()
    if bpy.app.timers.is_registered(_tick):bpy.app.timers.unregister(_tick)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_cancel_background);atexit.unregister(_shutdown)
    bpy.utils.unregister_class(PROJECT_DAMAGE_OT_continue_effect)
