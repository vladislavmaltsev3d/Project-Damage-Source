"""Check native imports using the same Python ABI as the target Blender."""
import importlib
import sys
from pathlib import Path

folder = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(folder))
checks = {
    "project_damage_core_py": ("build_brick_surface", "build_lath_surface", "build_rebar_surface_path"),
    "project_damage_peeling": ("peeling_generate", "peeling_restore_normals"),
    "project_damage_cellcuts": (),
}
for name, functions in checks.items():
    module = importlib.import_module(name)
    for function in functions:
        if not callable(getattr(module, function, None)):
            raise RuntimeError(f"Missing export: {name}.{function}")
    print(name, "OK", getattr(module, "__version__", ""))
