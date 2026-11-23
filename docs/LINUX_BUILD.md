# MeshSimplify: Linux build

## Status

| | |
|---|---|
| Windows x64 | Built and tested headless (`scripts\test_all.ps1`) on 14.1v8, 15.0v8, 15.1v10, 15.2v9, 16.0v8, 16.1v1, 17.0v4, 17.1v1 |
| Linux x64 | Built with `scripts/build_linux_docker.sh` (Rocky Linux 8, gcc-toolset-11) from main @ db09b49 for all eight; needs only GLIBC_2.14 / GLIBCXX_3.4.21 (RHEL / Rocky 7+), no RPATH; `test/test_crop.py` 31/31 on every version (release `v1.1-linux`). `scripts/linux_compile_check.sh` remains the no-Linux-Nuke check from Windows / WSL. |

A Linux MODULE links happily with every DDImage symbol undefined, so
`linux_compile_check.sh` alone proves the code, the CMake Linux settings and
the install layout -- not that Nuke loads the result (NukeDevRules
NDK_NOTES 57). Ship Linux binaries only from the container build below,
against real Linux Nuke installs.

## Release build

On a Linux host with Docker or Podman and the Linux Nuke installs (only
`include/` and `cmake/` of each are needed) under `~/Nuke/Nuke<ver>/`:

```bash
scripts/build_linux_docker.sh                 # every Nuke<ver>/ under ~/Nuke
scripts/build_linux_docker.sh 14.1v8 16.0v8   # or just these
NUKE_ROOT=/opt/nukes DOCKER=podman scripts/build_linux_docker.sh
```

It runs `scripts/build_all.sh` in a Rocky Linux 8 container with
`gcc-toolset-11` and prints the highest GLIBC / GLIBCXX each `.so` needs
(must be <= GLIBC_2.28 / GLIBCXX_3.4.25 to load on Rocky / RHEL 8). Output:
`linux-bin/<major.minor>/MeshSimplify.so` and `linux-bin.tar.gz`. The build
fetches meshoptimizer from GitHub (CMake FetchContent), so it needs network
access once per build dir.

Then on Windows, in the repo root:

```powershell
tar -xzf linux-bin.tar.gz
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.0
```

which produces `dist\MeshSimplify-<version>-win64-linux64.zip`; the `.so` sits
next to the `.dll` in each `<major.minor>/` folder and each Nuke loads only
its own platform's binary.

A quick native build (developer machine, not for distribution):
`scripts/build_all.sh [--install] [ver ...]`. On a new distro this links
against a newer glibc than studios run (`GLIBC_2.38 not found` on Rocky 8).

## Toolchain and ABI

| | |
|---|---|
| Compiler | GCC 11 (`gcc-toolset-11`); Foundry asks for GCC 9.3-9.5 for 14.x and 11.2 for 15-17 |
| libstdc++ string ABI | Nuke 14.x: `_GLIBCXX_USE_CXX11_ABI=0` (set by CMakeLists.txt). 15.0+: default new ABI |
| Flags | `-Wall -Wreorder -Wno-deprecated-declarations`, PIC, Release by default |
| meshoptimizer | static, PIC, linked with `-Wl,--exclude-libs,ALL` so its symbols stay private to the `.so` |
| Output name | `MeshSimplify.so` (CMake `PREFIX ""`; Nuke needs the file name to match the class) |
| RPATH | none (`SKIP_BUILD_RPATH`): libDDImage comes from the running Nuke |
| STLAllocator.h | NDK <= 15.x ships a header GCC 14 rejects; CMake shadows it with a fixed copy |

If a 14.1 render fails to load the plugin while 15+ is fine, rebuild 14.1
with GCC 9 (`gcc-toolset-9`) as Foundry asks.

## Install / farm

Same layout as Windows: `MeshSimplify/init.py` adds `MeshSimplify/<major.minor>/`
for the running Nuke. `cmake --install <build> --prefix ~/.nuke` also
registers `nuke.pluginAddPath('./MeshSimplify')` in `~/.nuke/init.py`. Farm
jobs usually do not read your `~/.nuke`: put the folder on a shared path in
the farm's `NUKE_PATH`. "MeshSimplify: Unknown command" in a render log means
the folder (or the binary for that Nuke version) is not on the plugin path;
`undefined symbol` / `wrong ELF class` is a real toolchain mismatch.
