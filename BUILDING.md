# Building Project Damage from source

## Requirements

- CMake 3.24 or newer and a C++17 compiler.
- macOS: Xcode Command Line Tools; Apple Silicon for the supplied default loader path.
- Windows: Visual Studio 2022 Build Tools with Desktop development with C++, Windows SDK and a 64-bit CPython development installation.
- CPython 3.11 or newer, with development headers, matching the exact major/minor ABI and CPU architecture of the target Blender. This snapshot has been used with Blender 5.2.2 / CPython 3.13 and earlier CPython 3.11 builds.
- NumPy in the verification interpreter for array-based generator calls. Blender normally supplies it.

Blender may omit Python development headers. In that case use a matching CPython development installation, or pass `-DPython_INCLUDE_DIR=/path/to/matching/python/include` with headers and `pyconfig.h` from that same Python version and platform.

No Cinema 4D SDK is required. Source dependencies are bundled in `third_party/` and are extracted with checksum verification. Their licenses are preserved inside the archives and copied to `licenses/`.

## macOS

From this repository folder (replace the Python path with your own):

```sh
cmake -S . -B build-cp313 -DCMAKE_BUILD_TYPE=Release \
  -DPython_EXECUTABLE="/path/to/python3.13" -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-cp313 --config Release --parallel 2 \
  --target project_damage_core_py project_damage_peeling project_damage_cellcuts
cmake --install build-cp313 --config Release --component native --prefix "$PWD/build-package"
```

The resulting modules are in `build-package/project_damage/core/macos-arm64-cp313/`. Build against Python 3.11 in a different build directory for `cp311`. Do not rename a binary to pretend it has a different Python ABI.

## Windows

Run in a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build-cp313 -G "Visual Studio 17 2022" -A x64 -DPython_EXECUTABLE="C:/Python313/python.exe"
cmake --build build-cp313 --config Release --parallel 2 --target project_damage_core_py project_damage_peeling project_damage_cellcuts
cmake --install build-cp313 --config Release --component native --prefix build-package
```

Modules are installed to `build-package/project_damage/core/win64-cp313/`. The C++ runtime is statically linked to avoid depending on a different MSVC runtime already loaded by Blender. Windows build instructions are provided, but this source snapshot has not been compiled or tested on Windows in this publication task.

Linux can compile the native targets with CMake, but this add-on snapshot's automatic loader does not implement a Linux platform directory. Linux add-on deployment is not advertised as tested or supported by this snapshot.

## Verify and use

```sh
/path/to/python3.13 tools/verify_native.py build-package/project_damage/core/macos-arm64-cp313
```

For Windows, use the corresponding interpreter and `win64-cp313` folder. The verification script imports all three extension modules and checks representative generator exports.

Keep the `.py` modules and native modules from the same source snapshot. Install the built modules into the matching `project_damage/core/<platform>-cp<ABI>/` folder of an add-on copy, then restart Blender. Do not overwrite a module while Blender has it loaded. The addon supports a custom core directory for development. Optional `.blend` material libraries, icons, previews and rendered manual assets come from the corresponding product package; they are not C++ build inputs.

`project_damage_peeling` is the exported module name. Some development distributions rename its file to `project_damage_peeling_runtime2...`; the existing Python loader accepts either filename.

## What changed for publication

Generator algorithms and all installed Python modules are copied unchanged. The root CMake build replaces old host-specific scripts that referenced absent local files. Duplicate source entries were removed from the build list. Dependency source archives preserve the installed Manifold tree and the local sources used for oneTBB, Clipper2 and nanobind/robin-map. Build products are never committed.

## Validation of this snapshot

On 14 November 2026 the three native targets built successfully on macOS arm64 with AppleClang 21 and CPython 3.13.13, and passed `tools/verify_native.py`. All 187 add-on Python modules passed syntax parsing and match the installed source snapshot. This is a build/import check, not an exhaustive functional test of every generator.
