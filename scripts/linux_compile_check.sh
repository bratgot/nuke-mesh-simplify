#!/usr/bin/env bash
# Linux configure + compile + link + install check WITHOUT a Linux Nuke
# (NukeDevRules NDK_NOTES 57 / 67). Runs the real CMakeLists.txt with a stub
# NukeConfig.cmake whose Nuke::NDK target points at the WINDOWS install's
# headers (DDImage headers are portable C++) and links no DDImage library:
# a Linux MODULE links fine with those symbols undefined.
#
# Proves: the code compiles with g++, CMake's Linux settings (MeshSimplify.so
# name, PIC static meshoptimizer, old string ABI for Nuke < 15, Release, no
# RPATH, private meshopt symbols) and the install layout.
# Does NOT prove the plugin loads in Linux Nuke. Release .so files come from
# build_linux_docker.sh against real Linux Nuke installs.
#
# Run in WSL (e.g. AlmaLinux 9: g++ 11.5, cmake 3.26) from anywhere:
#   bash scripts/linux_compile_check.sh [ver ...]
# Uses build-*/_deps/meshoptimizer-src from a Windows configure if present
# (no network), else lets FetchContent clone it.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
win_pf="${WIN_PF:-/mnt/c/Program Files}"
work="${WORK:-/tmp/meshsimplify-linuxcheck}"
versions=("$@")
[ ${#versions[@]} -eq 0 ] && versions=(14.1v8 15.0v8 15.1v10 15.2v9 16.0v8 16.1v1 17.0v4 17.1v1)
mo="$(ls -d "$root"/build-*/_deps/meshoptimizer-src 2>/dev/null | head -1)"
fc=(); [ -n "$mo" ] && fc=(-DFETCHCONTENT_SOURCE_DIR_MESHOPTIMIZER="$mo")
echo "compiler: $(${CXX:-g++} --version | head -1); $(cmake --version | head -1)"
results=()
for v in "${versions[@]}"; do
    inc="$win_pf/Nuke$v/include"
    [ -f "$inc/DDImage/GeoOp.h" ] || { results+=("SKIP  $v (no $inc)"); continue; }
    stub="$work/Nuke$v/cmake"; bld="$work/build-$v"; pre="$work/prefix-$v"
    rm -rf "$bld" "$pre"; mkdir -p "$stub"
    cat > "$stub/NukeConfig.cmake" <<EOC
add_library(Nuke::NDK INTERFACE IMPORTED)
set_target_properties(Nuke::NDK PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$inc")
EOC
    echo "=== Nuke $v ==="
    log="$work/log-$v.txt"
    if cmake -S "$root" -B "$bld" -DNuke_DIR="$stub" "${fc[@]}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$log" 2>&1 &&
       cmake --build "$bld" -j"$(nproc)" >>"$log" 2>&1 &&
       cmake --install "$bld" --prefix "$pre" >>"$log" 2>&1; then
        so="$bld/MeshSimplify.so"
        leaked=$(nm -D --defined-only "$so" | grep -c " meshopt_" || true)
        rpath=$(readelf -d "$so" | grep -ciE "rpath|runpath" || true)
        abi=$(grep -c "_GLIBCXX_USE_CXX11_ABI=0" "$bld/compile_commands.json" || true)
        own=$(grep -E "NukeMeshSimplify\.cpp:[0-9]+:[0-9]+: (warning|error)" "$log" | wc -l)
        mm="${v%%v*}"
        inst=$([ -f "$pre/MeshSimplify/$mm/MeshSimplify.so" ] && [ -f "$pre/MeshSimplify/init.py" ] && echo ok || echo MISSING)
        results+=("OK    $v  meshopt exports=$leaked rpath=$rpath old-abi-TUs=$abi own-warnings=$own install=$inst")
    else
        results+=("FAIL  $v (see $log)")
        grep -E "error" "$log" | head -10
    fi
done
echo; echo "==== linux_compile_check summary (compiles; NOT load-tested) ===="
printf '%s\n' "${results[@]}"
case " ${results[*]} " in *"FAIL "*) exit 1 ;; esac
exit 0
