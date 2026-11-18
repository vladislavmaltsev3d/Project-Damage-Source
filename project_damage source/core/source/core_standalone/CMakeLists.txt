# описываем исходники и библиотеки для сборки ядра
# Standalone proof target for the host-neutral ProjectDamage core (core-standalone arc,
# 2026-08-13). It compiles the four core TUs (project_damage_core, project_damage_break_geo, project_damage_cache, project_damage_fracture) against nothing but
# Manifold, TBB and the standard library - no Cinema 4D SDK on the include path
# at all - which is what turns "no c4d/maxon symbol appears in these files" into
# "these files do not NEED c4d.h". The static library itself is never shipped or
# linked into anything; the build succeeding IS the deliverable.
#
# PROJECT_DAMAGE_STANDALONE is defined HERE and nowhere else, ever: it makes project_damage_core.h
# drop its c4d.h include, expand the namespace wrap to nothing and supply the
# scalar shims (Int32/Float/Bool/NOTOK/LIMIT) the SDK otherwise provides.
#
# Plain MSVC defaults on purpose - no SDK compiler flags - so new diagnostics
# here are information about the core, not noise to be silenced.
cmake_minimum_required(VERSION 3.20)
project(project_damage_core_standalone CXX)

# Static C++ runtime for this whole stack, project_damage_core_py included.
#
# The module is loaded into HOST processes we do not control. Blender bundles
# its own msvcp140.dll app-local and loads it at startup, before any addon
# runs: 14.29 (VS2019-era) on Blender 4.5 LTS, 14.44 on 5.2. Windows resolves
# a DLL import by base name against what is already loaded, so a dynamic-CRT
# .pyd binds to whatever version the host happens to ship - and against 14.29
# the module crashed on its first call (EXCEPTION_ACCESS_VIOLATION inside
# MSVCP140.dll). There is no Python-side way to redirect that. A statically
# linked CRT removes the import entirely and makes the module self-contained
# on every host, present and future.
#
# The Cinema 4D plugin builds are NOT affected: they use their own project
# files against cpp/manifold/build, and keep /MD as C4D requires. The deps
# this stack links come from the parallel /MT tree cpp/manifold/build_mt,
# built by cpp/setup_deps.ps1 - mixing CRTs within one binary would not link.
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# stdcpp17, matching sdk/<gen>/frameworks/settings/Base.props (the C4D builds'
# own standard).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_DAMAGE_CPP "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# One dependency tree per platform, both built from the same pinned manifold
# source (cpp/manifold/PINNED_TAG.txt): build_mt is the Windows static-CRT
# tree (setup_deps.ps1), build_mac the macOS one (build_core_mac.sh).
if(APPLE)
	set(PROJECT_DAMAGE_DEP_TREE "${PROJECT_DAMAGE_CPP}/manifold/build_mac")
	# arm64 determinism: clang contracts fp expressions by default on arm64;
	# research/07 names this as the one known float landmine, so it is off for
	# every TU of this stack. ONE source for the flag - cpp/project_damage_float_flags.txt
	# - which build_mac_deps.sh reads for the dependency tree, build_project_damage_mac.sh
	# for the plugin and the RNG self-test, and this target here.
	file(READ "${PROJECT_DAMAGE_CPP}/project_damage_float_flags.txt" PROJECT_DAMAGE_FP_FLAGS)
	string(STRIP "${PROJECT_DAMAGE_FP_FLAGS}" PROJECT_DAMAGE_FP_FLAGS)
	add_compile_options(${PROJECT_DAMAGE_FP_FLAGS})
else()
	set(PROJECT_DAMAGE_DEP_TREE "${PROJECT_DAMAGE_CPP}/manifold/build_mt")
endif()

# The same fetched TBB source tree Manifold builds its builtin oneTBB from, and
# the same one cpp/projectdefinition.txt points the C4D builds at - project_damage_host.h
# includes <tbb/parallel_for.h> directly.
set(PROJECT_DAMAGE_TBB_INCLUDE "${PROJECT_DAMAGE_DEP_TREE}/_deps/tbb-src/include")
if(NOT EXISTS "${PROJECT_DAMAGE_TBB_INCLUDE}/tbb/parallel_for.h")
	message(FATAL_ERROR "TBB headers not found at ${PROJECT_DAMAGE_TBB_INCLUDE} - run cpp/setup_deps.ps1 first")
endif()

set(PROJECT_DAMAGE_CLIPPER2_INCLUDE "${PROJECT_DAMAGE_DEP_TREE}/_deps/clipper2-src/CPP/Clipper2Lib/include")
if(NOT EXISTS "${PROJECT_DAMAGE_CLIPPER2_INCLUDE}/clipper2/clipper.h")
	message(FATAL_ERROR "Clipper2 headers not found at ${PROJECT_DAMAGE_CLIPPER2_INCLUDE} - run cpp/setup_deps.ps1 first")
endif()

# Shared by project_damage_core and project_damage_core_py below - one list, so the two targets
# cannot drift apart as Task 2 grows what the module TU includes.
set(PROJECT_DAMAGE_CORE_INCLUDES
	"${PROJECT_DAMAGE_CPP}/source"
	"${PROJECT_DAMAGE_CPP}/res/description"
	"${PROJECT_DAMAGE_CPP}/manifold/include"
	"${PROJECT_DAMAGE_CLIPPER2_INCLUDE}"
	"${PROJECT_DAMAGE_TBB_INCLUDE}")

add_library(project_damage_core STATIC
    "${PROJECT_DAMAGE_CPP}/source/peeling.cpp"
    "${PROJECT_DAMAGE_CPP}/source/damage_shading.cpp"
    "${PROJECT_DAMAGE_CPP}/source/break_rebar.cpp"
    "${PROJECT_DAMAGE_CPP}/source/break_framing.cpp"
    "${PROJECT_DAMAGE_CPP}/source/break_chips.cpp"
    "${PROJECT_DAMAGE_CPP}/source/break_texture.cpp"
    "${PROJECT_DAMAGE_CPP}/source/edge_bevel.cpp"
    "${PROJECT_DAMAGE_CPP}/source/damaged_bevel.cpp"
    "${PROJECT_DAMAGE_CPP}/source/scratches_draw.cpp"
    "${PROJECT_DAMAGE_CPP}/source/metal_deform.cpp"
    "${PROJECT_DAMAGE_CPP}/source/bullet_impacts.cpp"
    "${PROJECT_DAMAGE_CPP}/source/building_destruction.cpp"
    "${PROJECT_DAMAGE_CPP}/source/ground_cracks.cpp"
    "${PROJECT_DAMAGE_CPP}/source/brick_damage.cpp"
    "${PROJECT_DAMAGE_CPP}/source/broken_glass.cpp"
    "${PROJECT_DAMAGE_CPP}/source/broken_glass_legacy.cpp"
    "${PROJECT_DAMAGE_CPP}/source/ground_fissure.cpp"
    "${PROJECT_DAMAGE_CPP}/source/rusty_holes.cpp"
	"${PROJECT_DAMAGE_CPP}/source/coating_peeling.cpp"
    "${PROJECT_DAMAGE_CPP}/source/rust.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_flat_legacy.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_solid_legacy.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_tears_legacy.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_tears.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_flat.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_solid.cpp"
    "${PROJECT_DAMAGE_CPP}/source/hang_hole.cpp"
    "${PROJECT_DAMAGE_CPP}/source/adaptive_refine.cpp"
    "${PROJECT_DAMAGE_CPP}/source/concrete_damage.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_flat.cpp"
    "${PROJECT_DAMAGE_CPP}/source/fabric_solid.cpp"
    "${PROJECT_DAMAGE_CPP}/source/hang_hole.cpp"
    "${PROJECT_DAMAGE_CPP}/source/impact_deform.cpp"
    "${PROJECT_DAMAGE_CPP}/source/ivy_generator.cpp"
    "${PROJECT_DAMAGE_CPP}/source/blast_crater.cpp"
    "${PROJECT_DAMAGE_CPP}/source/gen_metal_tear.cpp"
    "${PROJECT_DAMAGE_CPP}/source/tile_damage.cpp"
    "${PROJECT_DAMAGE_CPP}/source/wood_split.cpp"
    "${PROJECT_DAMAGE_CPP}/source/erosion.cpp"
    "${PROJECT_DAMAGE_CPP}/source/roots_generator.cpp"
    "${PROJECT_DAMAGE_CPP}/source/cable_generator.cpp"
    "${PROJECT_DAMAGE_CPP}/source/paper_generator.cpp"
    "${PROJECT_DAMAGE_CPP}/source/cloth_sheets.cpp"
    "${PROJECT_DAMAGE_CPP}/source/wall_hole.cpp"
    "${PROJECT_DAMAGE_CPP}/source/adaptive_refine.cpp"
    "${PROJECT_DAMAGE_CPP}/source/concrete_hit.cpp"
    "${PROJECT_DAMAGE_CPP}/source/damage_debris.cpp"
    "${PROJECT_DAMAGE_CPP}/source/rb_fracture.cpp"
    "${PROJECT_DAMAGE_CPP}/source/cell_fracture.cpp"
    "${PROJECT_DAMAGE_CPP}/source/plaster_displace.cpp"
    "${PROJECT_DAMAGE_CPP}/source/concrete_pits.cpp"
    "${PROJECT_DAMAGE_CPP}/source/metal_tear.cpp"
    "${PROJECT_DAMAGE_CPP}/source/clay.cpp"
    "${PROJECT_DAMAGE_CPP}/source/voxel_erosion.cpp"
    "${PROJECT_DAMAGE_CPP}/source/weld.cpp"
    "${PROJECT_DAMAGE_CPP}/source/foam.cpp"
    "${PROJECT_DAMAGE_CPP}/source/grass_scatter.cpp"
    "${PROJECT_DAMAGE_CPP}/source/wood_damage.cpp"
    "${PROJECT_DAMAGE_CPP}/source/lath_damage.cpp"
    "${PROJECT_DAMAGE_CPP}/source/wallpaper.cpp"
    "${PROJECT_DAMAGE_CPP}/source/heat_damage.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_deposits.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_core.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_break_geo.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_cache.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_fracture.cpp"
	"${PROJECT_DAMAGE_CPP}/source/project_damage_fracture_volume.cpp")

target_compile_definitions(project_damage_core PRIVATE PROJECT_DAMAGE_STANDALONE)

# res/description carries obreaker.h, a plain enum of the Breaker's parameter
# ids that project_damage_core.cpp reads for BREAKER_MODE_* - no C4D dependency in it.
target_include_directories(project_damage_core PRIVATE ${PROJECT_DAMAGE_CORE_INCLUDES})

# --- project_damage_core_py: the same source tree as a compiled Python extension ------
# (nanobind-module arc, Task 1, 2026-08-13). Entirely OPTIONAL: unlike
# project_damage_core above (which needs only headers), this target is a real DLL that
# must resolve every symbol at link time and needs a working Python dev
# environment, so its absence must not break the project_damage_core static-lib proof -
# find_package is deliberately non-REQUIRED and everything module-specific is
# gated on Python_FOUND. Python_EXECUTABLE is normally passed in by
# build_core_standalone.ps1, which probes the machine for a CPython >= 3.11
# with dev headers first (spec risk 1); plain `cmake -S` without that flag
# falls back to FindPython's own search.
find_package(Python 3.11 COMPONENTS Interpreter Development.Module)

if(Python_FOUND)
	# A real DLL needs the ACTUAL compiled Manifold/Clipper2/TBB static libs -
	# the same ones cpp/projectdefinition.txt links for the C4D generations -
	# not just their headers.
	if(APPLE)
		# The macOS dep tree's layout is CMake's business, not ours - the
		# authoring guess (lib/, _deps/clipper2-build/, *_release/) was wrong on
		# the very first CI run, where libmanifold.a came out in src/. So each
		# archive is SEARCHED for rather than located by a hardcoded path: the
		# tree is built by cpp/build_core_mac.sh immediately before this, in
		# Release only, so there is exactly one of each, and the STATUS lines
		# record the real layout in the build log. The Windows branch below
		# keeps its explicit paths - they are proven, and the 'mt' token in the
		# oneTBB directory name is load-bearing evidence there (see below).
		function(project_damage_find_static_lib out name)
			file(GLOB_RECURSE hits "${PROJECT_DAMAGE_DEP_TREE}/${name}")
			if(NOT hits)
				message(FATAL_ERROR "${name} not found under ${PROJECT_DAMAGE_DEP_TREE} - run cpp/build_core_mac.sh first")
			endif()
			list(GET hits 0 first)
			get_filename_component(dir "${first}" DIRECTORY)
			message(STATUS "project_damage: ${name} -> ${dir}")
			set(${out} "${dir}" PARENT_SCOPE)
		endfunction()
		project_damage_find_static_lib(PROJECT_DAMAGE_MANIFOLD_LIB_DIR libmanifold.a)
		project_damage_find_static_lib(PROJECT_DAMAGE_CLIPPER2_LIB_DIR libClipper2.a)
		project_damage_find_static_lib(PROJECT_DAMAGE_TBB_LIB_DIR libtbb.a)
		set(PROJECT_DAMAGE_TBB_LINK tbb tbbmalloc)
	else()
		set(PROJECT_DAMAGE_MANIFOLD_LIB_DIR "${PROJECT_DAMAGE_DEP_TREE}/lib/Release")
		set(PROJECT_DAMAGE_CLIPPER2_LIB_DIR "${PROJECT_DAMAGE_DEP_TREE}/_deps/clipper2-build/Release")
		file(GLOB PROJECT_DAMAGE_TBB_LIB_DIRS "${PROJECT_DAMAGE_DEP_TREE}/msvc_*_mt_release")
		set(PROJECT_DAMAGE_TBB_LINK tbb12 tbbmalloc)
	endif()

	# APPLE needs no existence check: project_damage_find_static_lib above already fails
	# hard when an archive is missing, and it is the same search that produced
	# the directory being checked.
	if(NOT APPLE)
		if(NOT EXISTS "${PROJECT_DAMAGE_MANIFOLD_LIB_DIR}/manifold.lib")
			message(FATAL_ERROR "manifold.lib not found at ${PROJECT_DAMAGE_MANIFOLD_LIB_DIR} - run cpp/setup_deps.ps1 first")
		endif()
		if(NOT EXISTS "${PROJECT_DAMAGE_CLIPPER2_LIB_DIR}/Clipper2.lib")
			message(FATAL_ERROR "Clipper2.lib not found at ${PROJECT_DAMAGE_CLIPPER2_LIB_DIR} - run cpp/setup_deps.ps1 first")
		endif()
	endif()

	# Windows only - macOS resolved its own TBB directory by search above.
	# oneTBB names its output directory after the compiler AND the CRT it was
	# built with, so the static-CRT tree gives msvc_<ver>_cxx17_64_mt_release.
	# Globbed rather than hardcoded: the version moves with the toolchain, and
	# the 'mt' token is oneTBB's own confirmation that it honored
	# CMAKE_MSVC_RUNTIME_LIBRARY (an _md_release match here would mean the
	# module still imports msvcp140).
	if(NOT APPLE)
		if(NOT PROJECT_DAMAGE_TBB_LIB_DIRS)
			message(FATAL_ERROR "no static-CRT oneTBB directory (msvc_*_mt_release) under ${PROJECT_DAMAGE_DEP_TREE} - run cpp/setup_deps.ps1 first")
		endif()
		list(GET PROJECT_DAMAGE_TBB_LIB_DIRS 0 PROJECT_DAMAGE_TBB_LIB_DIR)
		if(NOT EXISTS "${PROJECT_DAMAGE_TBB_LIB_DIR}/tbb12.lib")
			message(FATAL_ERROR "tbb12.lib not found at ${PROJECT_DAMAGE_TBB_LIB_DIR} - run cpp/setup_deps.ps1 first")
		endif()
	endif()

	include(FetchContent)
	FetchContent_Declare(nanobind
		GIT_REPOSITORY https://github.com/wjakob/nanobind.git
		GIT_TAG v2.14.0) # latest tagged release as of 2026-08-13 (project has no separate GitHub Releases, only tags)
	FetchContent_MakeAvailable(nanobind)

	nanobind_add_module(project_damage_core_py NB_STATIC "${PROJECT_DAMAGE_CPP}/core_standalone/project_damage_core_py.cpp")
	nanobind_add_module(project_damage_peeling NB_STATIC
        "${PROJECT_DAMAGE_CPP}/source/peeling_module.cpp"
        "${PROJECT_DAMAGE_CPP}/source/peeling.cpp"
        "${PROJECT_DAMAGE_CPP}/source/peeling_full.cpp")
    target_include_directories(project_damage_peeling PRIVATE ${PROJECT_DAMAGE_CORE_INCLUDES})
    target_link_directories(project_damage_peeling PRIVATE "${PROJECT_DAMAGE_MANIFOLD_LIB_DIR}" "${PROJECT_DAMAGE_CLIPPER2_LIB_DIR}" "${PROJECT_DAMAGE_TBB_LIB_DIR}")
    target_link_libraries(project_damage_peeling PRIVATE manifold Clipper2 ${PROJECT_DAMAGE_TBB_LINK})
    nanobind_add_module(project_damage_cellcuts NB_STATIC "${PROJECT_DAMAGE_CPP}/source/cell_cut_module.cpp")
    target_include_directories(project_damage_cellcuts PRIVATE ${PROJECT_DAMAGE_CORE_INCLUDES})
    target_link_directories(project_damage_cellcuts PRIVATE "${PROJECT_DAMAGE_MANIFOLD_LIB_DIR}" "${PROJECT_DAMAGE_CLIPPER2_LIB_DIR}" "${PROJECT_DAMAGE_TBB_LIB_DIR}")
    target_link_libraries(project_damage_cellcuts PRIVATE manifold Clipper2 ${PROJECT_DAMAGE_TBB_LINK})

	# Shares PROJECT_DAMAGE_STANDALONE and the include dirs with project_damage_core above; the
	# module TU includes the whole core surface (project_damage_core.h: ProjectDamageParams,
	# BreakerSpec, CustomShape, the Cache, BuildInputManifold).
	target_compile_definitions(project_damage_core_py PRIVATE PROJECT_DAMAGE_STANDALONE)
	# Product version baked into project_damage_core_py.__version__; passed in by
	# build_core_standalone.ps1 from cpp/project_damage_manual_host.py's __version__.
	# Absent (plain `cmake -S` by hand) the module stamps "0.0.0-dev".
	if(DEFINED PROJECT_DAMAGE_CORE_VERSION)
		target_compile_definitions(project_damage_core_py PRIVATE PROJECT_DAMAGE_CORE_VERSION="${PROJECT_DAMAGE_CORE_VERSION}")
	endif()
	target_include_directories(project_damage_core_py PRIVATE ${PROJECT_DAMAGE_CORE_INCLUDES})
	target_link_directories(project_damage_core_py PRIVATE
		"${PROJECT_DAMAGE_MANIFOLD_LIB_DIR}"
		"${PROJECT_DAMAGE_CLIPPER2_LIB_DIR}"
		"${PROJECT_DAMAGE_TBB_LIB_DIR}")
	# project_damage_core (the static lib target above) is what actually DEFINES
	# BuildWornCore - Task 1's walking skeleton never called it, so this link
	# was never needed until Task 2's real surface started calling in.
	target_link_libraries(project_damage_core_py PRIVATE project_damage_core manifold Clipper2 ${PROJECT_DAMAGE_TBB_LINK})
else()
	message(STATUS "project_damage_core_py: skipped - no CPython >= 3.11 with Development.Module found. The project_damage_core static-lib proof is unaffected.")
endif()
