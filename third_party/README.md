# Dependency source archives

These archives contain source, headers, build scripts and license notices, not compiled libraries. CMake verifies their SHA-256 values and extracts them into the build directory. `manifest.json` records checksums.

| Component | Declared version | License | Upstream |
| --- | --- | --- | --- |
| Manifold | 3.5.2 | Apache-2.0 | https://github.com/elalish/manifold |
| oneTBB | 2022.3.0 | Apache-2.0 | https://github.com/uxlfoundation/oneTBB |
| Clipper2 | 2.0.1 | BSL-1.0 | https://github.com/AngusJohnson/Clipper2 |
| nanobind | 2.14.0 | BSD-3-Clause | https://github.com/wjakob/nanobind |
| robin-map (inside nanobind) | bundled snapshot | MIT | https://github.com/Tessil/robin-map |

Manifold is the source tree bundled with this Project Damage snapshot and may contain local changes; it is not claimed to be an unmodified upstream release. The other archives preserve the dependency sources used for this build. Git history, generated builds, compiled files and large example media are omitted. The exact archived sources, rather than a moving upstream branch, define this build. Full notices are inside the archives and in `../licenses/`.
