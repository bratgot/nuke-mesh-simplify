# MeshSimplify - Nuke NDK plugin (Nuke 14.1 - 17.1)
#   Windows: VS2019 x64, scripts\build_all.ps1 (one build-<ver>/ per Nuke)
#   Linux:   GCC, scripts/build_all.sh or scripts/build_linux_docker.sh
#
# Single version by hand:
#   cmake -G "Visual Studio 16 2019" -A x64 ^
#         -DNuke_DIR="C:/Program Files/Nuke16.0v8/cmake" -S . -B build-16.0v8
#   cmake --build build-16.0v8 --config Release
#   cmake --install build-16.0v8 --config Release --prefix "%USERPROFILE%/.nuke"
#
# The binary installs into MeshSimplify/<major.minor>/ and the shipped init.py
# adds the folder matching the running Nuke at startup.

cmake_minimum_required(VERSION 3.16)
project(MeshSimplify CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)   # single-config (Linux)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The static meshoptimizer is linked into a shared module: on Linux it must be
# position independent or the link fails (relocation R_X86_64_32 ...).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Several Nukes are installed side by side; newer ones register themselves in
# the CMake user-package registry and would hijack find_package (NDK_NOTES 11.2).
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
find_package(Nuke REQUIRED)

# major.minor of the target Nuke for the per-version install subfolder.
if(NOT MESHSIMPLIFY_NUKE_MM)
    string(REGEX MATCH "Nuke([0-9]+\\.[0-9]+)v" _ms_match "${Nuke_DIR}")
    set(MESHSIMPLIFY_NUKE_MM "${CMAKE_MATCH_1}")
endif()
if(NOT MESHSIMPLIFY_NUKE_MM)
    message(FATAL_ERROR
        "Cannot parse Nuke major.minor from Nuke_DIR='${Nuke_DIR}'; "
        "pass -DMESHSIMPLIFY_NUKE_MM=<major.minor>")
endif()
message(STATUS "MeshSimplify: targeting Nuke ${MESHSIMPLIFY_NUKE_MM}")

# ---------------------------------------------------------------------------
# meshoptimizer (MIT), fetched as source and linked statically so there is no
# extra runtime dependency to ship.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v0.25
)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_DEMO        OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK    OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL           OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(meshoptimizer)

# ---------------------------------------------------------------------------
# The plugin. MODULE (Nuke dlopens it; nothing links against it). The file
# name must match the Op::Description name (NDK_NOTES 1.7): PREFIX "" stops
# CMake naming it libMeshSimplify.so on Linux; the suffix is left to CMake
# (.dll / .so). No RPATH: libDDImage comes from the running Nuke.
# ---------------------------------------------------------------------------
add_library(MeshSimplify MODULE src/NukeMeshSimplify.cpp)
set_target_properties(MeshSimplify PROPERTIES
  OUTPUT_NAME "MeshSimplify"
  PREFIX ""
  SKIP_BUILD_RPATH ON
)
target_link_libraries(MeshSimplify PRIVATE Nuke::NDK meshoptimizer)

if(WIN32)
  target_compile_definitions(MeshSimplify PRIVATE
    NOMINMAX
    _USE_MATH_DEFINES
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
  )
endif()

if(MSVC)
  # /W3: DDK headers warn under /W4. 4996: Foundry deprecations inside their
  # own headers (NDK_NOTES 37). The CRT (/MD) is CMake's default for Release.
  target_compile_options(MeshSimplify PRIVATE /W3 /EHsc /wd4996)
else()
  target_compile_options(MeshSimplify PRIVATE -Wall -Wreorder -Wno-deprecated-declarations)
  # Keep the bundled meshoptimizer symbols private to this .so so another
  # plugin carrying its own copy cannot interpose them.
  target_link_options(MeshSimplify PRIVATE "LINKER:--exclude-libs,ALL")
  # Linux Nuke <= 14.x is built with the pre-C++11 libstdc++ string ABI.
  if(MESHSIMPLIFY_NUKE_MM VERSION_LESS 15.0)
    target_compile_definitions(MeshSimplify PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
  endif()
endif()

# NDK <= 15.x STLAllocator.h copy constructors assign INTO the const source.
# Old compilers ignored it in the uninstantiated template; GCC 14 rejects it.
# Fixed by Foundry in 16.0. Shadow the header with a corrected copy (inline
# template only, ABI-neutral). Same fix as ScreenspaceToTracker.
get_target_property(_ms_ndk_inc Nuke::NDK INTERFACE_INCLUDE_DIRECTORIES)
foreach(_inc IN LISTS _ms_ndk_inc)
  set(_stl "${_inc}/DDImage/STLAllocator.h")
  if(EXISTS "${_stl}")
    file(READ "${_stl}" _stl_src)
    string(FIND "${_stl_src}" "other._AllocatorInstance = _AllocatorInstance;" _bad)
    if(NOT _bad EQUAL -1)
      string(REPLACE "other._AllocatorInstance = _AllocatorInstance;"
                     "_AllocatorInstance = other._AllocatorInstance;" _stl_src "${_stl_src}")
      file(WRITE "${CMAKE_BINARY_DIR}/ndk_fix/DDImage/STLAllocator.h" "${_stl_src}")
      target_include_directories(MeshSimplify BEFORE PRIVATE "${CMAKE_BINARY_DIR}/ndk_fix")
      message(STATUS "MeshSimplify: using patched DDImage/STLAllocator.h")
    endif()
    break()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Install (prefix = the .nuke folder):
#   MeshSimplify/init.py, menu.py, LICENSE, ABOUT.txt
#   MeshSimplify/<major.minor>/MeshSimplify.{dll,so}
# plus an idempotent pluginAddPath block in <prefix>/init.py (NDK_NOTES 8.4).
# ---------------------------------------------------------------------------
install(TARGETS MeshSimplify
        LIBRARY DESTINATION MeshSimplify/${MESHSIMPLIFY_NUKE_MM}
        RUNTIME DESTINATION MeshSimplify/${MESHSIMPLIFY_NUKE_MM})
install(FILES python/init.py python/menu.py DESTINATION MeshSimplify)
foreach(_doc IN ITEMS LICENSE ABOUT.txt README.md)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}" DESTINATION MeshSimplify)
  endif()
endforeach()
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_user_init.cmake")
