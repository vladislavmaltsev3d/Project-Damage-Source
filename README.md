# Source Code

**Project Damage | Procedural Destruction Toolkit** — Python add-on and C++ geometry sources.

Maintained by **Vladislav Maltsev / Lazy3D**. Snapshot: **14 November 2026**, add-on version **0.15.49**. The date identifies this snapshot because several development updates share the same version number.

This repository contains the editable source for all generators in the installed add-on, including hidden and legacy implementations still referenced by the code. It contains no compiled native modules. This is a source distribution, not the complete ready-to-install product package.

## Contents

- `project_damage/*.py`: all 187 Python modules: interface, generators, drawing, materials, physics, live updates and background workers.
- `project_damage/core/source/source/`: geometry implementations and headers.
- `project_damage/core/source/core_standalone/`: the main Python/C++ binding.
- `CMakeLists.txt`: portable build for the main core, Peeling and Cell Cuts.
- `third_party/`: source-only dependency archives, checksums and attribution. CMake extracts them into its build folder; no dependency download is needed.
- `BUILDING.md`: build, verification and installation instructions.
- `LICENSE`, `CREDITS.txt`, `THIRD_PARTY_NOTICES.txt`, `licenses/`: licensing and upstream acknowledgments.
- `SOURCE_MANIFEST.json`: checksums of the published project sources.

Optional material libraries, material `.blend` files, preview packs, rendered documentation and user scenes are not included. They are not needed to compile the native modules. For the complete interface and bundled material assets, use the corresponding add-on distribution. User preferences, private presets, caches, build products and credentials are excluded.

## Build

See [BUILDING.md](BUILDING.md). Use a CPython development installation matching the major/minor version and architecture used by Blender. Python 3.11 and Python 3.13 require separate builds.

## License and origins

The Project Damage add-on and modified geometry sources are distributed under **GNU GPL version 3 or later**, as stated in their existing source headers. Third-party libraries retain their own licenses. This distribution includes a modified GPL geometry foundation credited to **Cornelius Dammrich**; its copyright and license notices are preserved. See [CREDITS.txt](CREDITS.txt) and [LICENSE](LICENSE).

This is a modified project, not an official upstream release or an endorsement by its upstream authors. No proprietary alternate license mentioned in original upstream headers is granted by this repository.

Material content is distributed separately; the software license does not automatically license unrelated assets. The current material workflow uses CC0 assets from ambientCG and Poly Haven.

## Contributions and repository access

Anyone can read, download and fork the source under its license. Only the repository owner and explicitly authorized collaborators can write to this repository. Proposed changes should be submitted as pull requests; the maintainer decides whether to merge them. There is no automatic merging or public write access. See [CONTRIBUTING.md](CONTRIBUTING.md).

[Tutorial playlist](https://www.youtube.com/playlist?list=PLbD5I1cRdfaU) · [Blender licensing information](https://www.blender.org/about/license/)
