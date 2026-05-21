# Nuke 14 NDK plugin: MeshSimplify
#
# Build:
#   cmake -G "Visual Studio 16 2019" -A x64 ^
#         -DCMAKE_PREFIX_PATH="C:/Program Files/Nuke14.0v5" ^
#         -DCMAKE_BUILD_TYPE=Release ^
#         -B build
#   cmake --build build --config Release
#
# Install:
#   copy build/Release/MeshSimplify.dll  %USERPROFILE%\.nuke\

cmake_minimum_required(VERSION 3.15)
project(MeshSimplify CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Nuke 14 NDK
#
# CMAKE_PREFIX_PATH must point at the Nuke install. NukeConfig.cmake handles
# include dirs, lib dirs, the compiler-version sanity check, and the
# version-mismatch tripwire.
# ---------------------------------------------------------------------------
find_package(Nuke REQUIRED)

# ---------------------------------------------------------------------------
# meshoptimizer (MIT)
#
# Fetched as source. It's a small C++ codebase with no transitive deps; we
# build it as a STATIC lib and link it into the plugin DLL so there's no
# extra runtime dependency to ship.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v0.25
)
# Force static, no testing, no installation.
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_DEMO        OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK    OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL           OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(meshoptimizer)

# ---------------------------------------------------------------------------
# The plugin target
#
# Build as MODULE: Nuke LoadLibrary's it; nothing else links against it.
# Producing an import lib would be wasted output and confuses CMake's
# default SHARED-library handling.
#
# OUTPUT_NAME must match the Op::Description name string declared in the
# source, exactly. Nuke's plugin loader uses the DLL filename to find the
# registration symbol; mismatch yields "plugin did not define MeshSimplify"
# at load time.
# ---------------------------------------------------------------------------
add_library(MeshSimplify MODULE
  src/NukeMeshSimplify.cpp
)

set_target_properties(MeshSimplify PROPERTIES
  OUTPUT_NAME "MeshSimplify"
  PREFIX ""
  SUFFIX ".dll"
)

target_link_libraries(MeshSimplify PRIVATE
  Nuke::NDK
  meshoptimizer
)

# ---------------------------------------------------------------------------
# MSVC defines and flags
#
# - NOMINMAX               : fnVC.h #errors without it
# - _USE_MATH_DEFINES      : fnVC.h #errors without it
# - WIN32_LEAN_AND_MEAN    : quieter windows.h
# - _CRT_SECURE_NO_WARNINGS: DDK uses strcpy/sprintf internally
# - /W3                    : DDK headers warn under /W4
# - /MD                    : match Nuke's CRT (dynamic, release)
# ---------------------------------------------------------------------------
if(MSVC)
  target_compile_definitions(MeshSimplify PRIVATE
    NOMINMAX
    _USE_MATH_DEFINES
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
  )
  target_compile_options(MeshSimplify PRIVATE
    /W3
    /EHsc
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/MD>
    $<$<CONFIG:Debug>:/MDd>
  )
endif()

# Optional: install step that drops the .dll into the user's .nuke folder.
# Comment out for CI / packaged builds.
if(DEFINED ENV{USERPROFILE})
  install(TARGETS MeshSimplify
    LIBRARY DESTINATION "$ENV{USERPROFILE}/.nuke"
    RUNTIME DESTINATION "$ENV{USERPROFILE}/.nuke"
  )
endif()
