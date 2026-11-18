# Source Code

**Project Damage | Procedural Destruction Toolkit** — Python add-on and C++ geometry sources.

Maintained by **Vladislav Maltsev / Lazy3D**. Release source snapshot: **Project Damage 1.0.0**, 17 November 2026.

This source distribution includes the released macOS Python add-on and the C++ sources for Core, Peeling and Cell Cuts. The Windows overlay reproduces the Windows release Python files and manifest. For a ready-to-install add-on, use the corresponding release package.

## Contents

- [`project_damage source/`](project_damage%20source): 193 Python modules from the macOS release.
- [`windows-x64-python.patch`](windows-x64-python.patch): Python and manifest changes for the Windows x64 release, which contains 194 Python modules.
- `project_damage source/core/source/source/`: geometry implementations and headers.
- `project_damage source/core/source/core_standalone/`: the main Python/C++ binding.
- `CMakeLists.txt`: portable build for the main core, Peeling and Cell Cuts.
- `third_party/`: source-only dependency archives, checksums and attribution. CMake extracts them into its build folder; no dependency download is needed.
- `BUILDING.md`: build, verification and installation instructions.
- `LICENSE`, `CREDITS.txt`, `THIRD_PARTY_NOTICES.txt`, `licenses/`: licensing and upstream acknowledgments.
- [`SOURCE_MANIFEST.json`](SOURCE_MANIFEST.json): source checksums and provenance for the two release archives.

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
