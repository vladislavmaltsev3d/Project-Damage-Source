# здесь фоновый процесс выполняет одну задачу и отдаёт результат
# SPDX-License-Identifier: GPL-3.0-or-later
"""Private one-job Blender process. No live scene or saved user file is opened."""
from pathlib import Path
import importlib,os,pickle,sys,time,traceback

def _resolve_package(package, path):
    # Set up the namespace in this private process before unpickling any jobs.
    if package.startswith('bl_ext.'):
        import bpy

        _, repository, _ = package.split('.')
        directory = path.parent
        repos = bpy.context.preferences.extensions.repos
        repo = next((item for item in repos if item.module == repository), None)
        if repo is not None and Path(repo.directory).resolve() != directory:
            repos.remove(repo)
            repo = None
        if repo is None:
            repos.new(name=repository, module=repository, custom_directory=str(directory), source='USER')
        else:
            repo.enabled = True
    else:
        sys.path.insert(0, str(path.parent))

    spec = importlib.util.find_spec(package)
    if spec is None or spec.origin is None or Path(spec.origin).resolve() != path / '__init__.py':
        raise ImportError(f'Background process cannot load {package} from {path}')

def main(folder,parent):

    pkg=Path(__file__).resolve().parent
    package = os.environ.get('PD_WORKER_PACKAGE', pkg.name)
    _resolve_package(package, pkg)
    sys.path.insert(0,str(pkg))
    from background_wire import decode
    from exact_cache import Results
    results=Results();cache=None;cache_owner=None
    allowed={'build_worn','grow_ivy','grow_roots','spray_foam','foam_path','scatter_grass','build_welds','compute_damage_shading','build_paper','apply_paper','build_rebar_surface_path'}
    tasks={'fabric_prep':{'prepare_with_mesh_prep','prepare_with_mesh_prep_fast'},'base_normals':{'compute_restore'},'python_jobs':{'compute'},'mesh_prep':{'prepare','is_closed_manifold'},'mesh_prep_fast':{'prepare','is_closed_manifold'},
           'break_surface':{'compute_cut_surface'},'cell_cut_plane':{'cut_mesh'},
           'cell_cuts':{'cut_paths'},'damage_shading':{'_rim_blend'},'input_prep':{'preserved_ngons'},'mesh_io':{'_scoped_shading'},'result_transfer':{'transfer_maps'},'inner_cutters':{'compute_interfaces','piece_maps'}}
    last=time.monotonic()
    while folder.is_dir():
        req=folder/'next'
        if not req.exists():
            if os.name=='posix' and os.getppid()!=parent:return
            # Poll quickly while a job is in flight, slowly once the worker sits idle.
            time.sleep(.018 if time.monotonic()-last<3. else .15);continue
        last=time.monotonic()
        try:
            with req.open('rb') as f:kind,module,fn,args,kw=pickle.load(f)
            req.unlink()
            memo=kw.pop('_pd_intermediate_cache',False)
            owner=kw.pop('_pd_owner',None)
            native_cache=kw.pop('_pd_native_cache',True)
            if not memo:results.clear()
            can_memo=memo and (kind=='native' or kind=='task' and module.rsplit('.',1)[-1]!='python_jobs')
            key=results.key(module,fn,args,kw) if can_memo else None
            hit,result=results.get(key)
            if kind=='native':
                if fn not in allowed:raise ValueError('Invalid native task')
                sys.path.insert(0,str(Path(module).parent))
                core=importlib.import_module('project_damage_core_py')
                args=decode(args,core);kw=decode(kw,core)
                if fn=='build_worn':
                    if not native_cache:cache=None
                    elif cache is None or owner!=cache_owner:cache=core.Cache();cache_owner=owner
                    kw['cache']=cache
                if not hit:result=getattr(core,fn)(*args,**kw)
                if fn=='build_worn' and cache is not None and hasattr(cache,'incremental_built') and not hit:
                    result['incremental_built']=cache.incremental_built
                    result['incremental_reused']=cache.incremental_reused
                if fn=='build_worn':result['intermediate_cache_hit']=hit
            else:
                task_package, _, short = module.rpartition('.')
                if task_package != package or fn not in tasks.get(short,()):
                    raise ValueError('Invalid background task: '+fn)
                if not hit:result=getattr(importlib.import_module(module),fn)(*args,**kw)
            if can_memo and not hit:results.put(key,result)
            response=(True,result)
        except Exception:response=(False,traceback.format_exc())
        with (folder/'result.tmp').open('wb') as f:pickle.dump(response,f,protocol=5)
        os.replace(folder/'result.tmp',folder/'reply')
        del response
        result=args=kw=None
        last=time.monotonic()

if __name__=='__main__':main(Path(sys.argv[-2]),int(sys.argv[-1]))
