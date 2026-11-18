# build the published sources without machine-specific folders
cmake_minimum_required(VERSION 3.24)
project(ProjectDamageSource VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(APPLE)
    add_compile_options(-ffp-contract=off)
endif()

# these archives contain source files only, with their original licenses
function(pd_unpack name expected)
    set(archive "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${name}-source.tar.gz")
    file(SHA256 "${archive}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Source archive checksum mismatch: ${name}")
    endif()
    set(base "${CMAKE_BINARY_DIR}/vendor/${actual}")
    if(NOT EXISTS "${base}/${name}/CMakeLists.txt")
        file(MAKE_DIRECTORY "${base}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${base}")
    endif()
    set(PD_VENDOR_${name} "${base}/${name}" PARENT_SCOPE)
endfunction()
pd_unpack(manifold 9c149991d9a72880ef8267c1860dfe8ca4e642b7b1df564a6f629d34e13a27ba)
pd_unpack(oneTBB 341650e1b66533e694a0ac2a562c1f380f616138e734c211d56862a328e3481e)
pd_unpack(Clipper2 3e4284786fae195c74c8f29101f85338c28afe1e9b85f25a6c43a1d4189b1422)
pd_unpack(nanobind a7708d7d45e7c1440e2a34823f8d73e774734e8249ff9c0063dfd69fdc18ce98)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static dependencies" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PAR ON CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION ON CACHE BOOL "" FORCE)
set(MANIFOLD_USE_BUILTIN_TBB ON CACHE BOOL "" FORCE)
set(MANIFOLD_USE_BUILTIN_CLIPPER2 ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_TBB "${PD_VENDOR_oneTBB}" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_CLIPPER2 "${PD_VENDOR_Clipper2}" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_NANOBIND "${PD_VENDOR_nanobind}" CACHE PATH "" FORCE)
add_subdirectory("${PD_VENDOR_manifold}" "vendor-build/manifold" EXCLUDE_FROM_ALL)
find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
add_subdirectory("${PD_VENDOR_nanobind}" "vendor-build/nanobind" EXCLUDE_FROM_ALL)

set(PD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/project_damage source/core/source")
add_library(project_damage_core STATIC
    "${PD_SOURCE}/source/peeling.cpp"
    "${PD_SOURCE}/source/damage_shading.cpp"
    "${PD_SOURCE}/source/break_rebar.cpp"
    "${PD_SOURCE}/source/break_framing.cpp"
    "${PD_SOURCE}/source/break_chips.cpp"
    "${PD_SOURCE}/source/break_texture.cpp"
    "${PD_SOURCE}/source/edge_bevel.cpp"
    "${PD_SOURCE}/source/damaged_bevel.cpp"
    "${PD_SOURCE}/source/scratches_draw.cpp"
    "${PD_SOURCE}/source/metal_deform.cpp"
    "${PD_SOURCE}/source/bullet_impacts.cpp"
    "${PD_SOURCE}/source/building_destruction.cpp"
    "${PD_SOURCE}/source/ground_cracks.cpp"
    "${PD_SOURCE}/source/brick_damage.cpp"
    "${PD_SOURCE}/source/broken_glass.cpp"
    "${PD_SOURCE}/source/broken_glass_legacy.cpp"
    "${PD_SOURCE}/source/ground_fissure.cpp"
    "${PD_SOURCE}/source/rusty_holes.cpp"
    "${PD_SOURCE}/source/coating_peeling.cpp"
    "${PD_SOURCE}/source/rust.cpp"
    "${PD_SOURCE}/source/fabric_flat_legacy.cpp"
    "${PD_SOURCE}/source/fabric_solid_legacy.cpp"
    "${PD_SOURCE}/source/fabric_tears_legacy.cpp"
    "${PD_SOURCE}/source/fabric_tears.cpp"
    "${PD_SOURCE}/source/fabric_flat.cpp"
    "${PD_SOURCE}/source/fabric_solid.cpp"
    "${PD_SOURCE}/source/hang_hole.cpp"
    "${PD_SOURCE}/source/adaptive_refine.cpp"
    "${PD_SOURCE}/source/concrete_damage.cpp"
    "${PD_SOURCE}/source/impact_deform.cpp"
    "${PD_SOURCE}/source/ivy_generator.cpp"
    "${PD_SOURCE}/source/blast_crater.cpp"
    "${PD_SOURCE}/source/gen_metal_tear.cpp"
    "${PD_SOURCE}/source/tile_damage.cpp"
    "${PD_SOURCE}/source/wood_split.cpp"
    "${PD_SOURCE}/source/erosion.cpp"
    "${PD_SOURCE}/source/roots_generator.cpp"
    "${PD_SOURCE}/source/cable_generator.cpp"
    "${PD_SOURCE}/source/paper_generator.cpp"
    "${PD_SOURCE}/source/cloth_sheets.cpp"
    "${PD_SOURCE}/source/wall_hole.cpp"
    "${PD_SOURCE}/source/concrete_hit.cpp"
    "${PD_SOURCE}/source/damage_debris.cpp"
    "${PD_SOURCE}/source/rb_fracture.cpp"
    "${PD_SOURCE}/source/cell_fracture.cpp"
    "${PD_SOURCE}/source/plaster_displace.cpp"
    "${PD_SOURCE}/source/concrete_pits.cpp"
    "${PD_SOURCE}/source/metal_tear.cpp"
    "${PD_SOURCE}/source/clay.cpp"
    "${PD_SOURCE}/source/voxel_erosion.cpp"
    "${PD_SOURCE}/source/weld.cpp"
    "${PD_SOURCE}/source/foam.cpp"
    "${PD_SOURCE}/source/grass_scatter.cpp"
    "${PD_SOURCE}/source/wood_damage.cpp"
    "${PD_SOURCE}/source/lath_damage.cpp"
    "${PD_SOURCE}/source/wallpaper.cpp"
    "${PD_SOURCE}/source/heat_damage.cpp"
    "${PD_SOURCE}/source/project_damage_deposits.cpp"
    "${PD_SOURCE}/source/project_damage_core.cpp"
    "${PD_SOURCE}/source/project_damage_break_geo.cpp"
    "${PD_SOURCE}/source/project_damage_cache.cpp"
    "${PD_SOURCE}/source/project_damage_fracture.cpp"
    "${PD_SOURCE}/source/project_damage_fracture_volume.cpp"
)
target_compile_definitions(project_damage_core PUBLIC PROJECT_DAMAGE_STANDALONE)
target_include_directories(project_damage_core PUBLIC
    "${PD_SOURCE}/source" "${PD_SOURCE}/res/description")
target_link_libraries(project_damage_core PUBLIC manifold Clipper2 TBB::tbb TBB::tbbmalloc)

nanobind_add_module(project_damage_core_py NB_STATIC
    "${PD_SOURCE}/core_standalone/project_damage_core_py.cpp")
target_compile_definitions(project_damage_core_py PRIVATE PROJECT_DAMAGE_CORE_VERSION="1.0.0")
target_link_libraries(project_damage_core_py PRIVATE project_damage_core)

nanobind_add_module(project_damage_peeling NB_STATIC
    "${PD_SOURCE}/source/peeling_module.cpp"
    "${PD_SOURCE}/source/peeling.cpp"
    "${PD_SOURCE}/source/peeling_full.cpp")
nanobind_add_module(project_damage_cellcuts NB_STATIC
    "${PD_SOURCE}/source/cell_cut_module.cpp")
foreach(module project_damage_peeling project_damage_cellcuts)
    target_compile_definitions(${module} PRIVATE PROJECT_DAMAGE_STANDALONE)
    target_include_directories(${module} PRIVATE "${PD_SOURCE}/source" "${PD_SOURCE}/res/description")
    target_link_libraries(${module} PRIVATE manifold Clipper2 TBB::tbb TBB::tbbmalloc)
endforeach()

if(APPLE)
    set(PD_PLATFORM "macos-arm64")
elseif(WIN32)
    set(PD_PLATFORM "win64")
else()
    set(PD_PLATFORM "linux-x64")
endif()
set(PD_CORE_SUBDIR "${PD_PLATFORM}-cp${Python_VERSION_MAJOR}${Python_VERSION_MINOR}"
    CACHE STRING "Subfolder in project_damage/core for this Python ABI")
install(TARGETS project_damage_core_py project_damage_peeling project_damage_cellcuts
    LIBRARY DESTINATION "project_damage/core/${PD_CORE_SUBDIR}" COMPONENT native
    RUNTIME DESTINATION "project_damage/core/${PD_CORE_SUBDIR}" COMPONENT native)
