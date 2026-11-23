#!/usr/bin/env bash
# Linux counterpart of build_all.ps1: builds MeshSimplify for every requested
# Nuke version into build-<ver>/, then stages linux-bin/<major.minor>/ and
# linux-bin.tar.gz for packaging on Windows (scripts\package.ps1).
#   scripts/build_all.sh [--install] [--clean] [ver ...]
# NUKE_ROOT (default ~/Nuke) holds the Nuke<ver>/ installs (only include/ and
# cmake/ are needed). With no versions given, every Nuke<ver>/ under NUKE_ROOT
# that has cmake/NukeConfig.cmake is built.
# For release binaries use scripts/build_linux_docker.sh (Rocky 8 / GCC 11):
# a plain build on a new distro links against a newer glibc than studios run.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
nuke_root="${NUKE_ROOT:-$HOME/Nuke}"
install=0; clean=0; versions=()
for a in "$@"; do
    case "$a" in
        --install) install=1 ;;
        --clean) clean=1 ;;
        *) versions+=("$a") ;;
    esac
done
if [ ${#versions[@]} -eq 0 ]; then
    for d in "$nuke_root"/Nuke*/; do
        [ -d "$d" ] || continue
        d="${d%/}"; versions+=("${d##*/Nuke}")
    done
fi
gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
results=(); built=()
for v in "${versions[@]}"; do
    cfg="$nuke_root/Nuke$v/cmake"
    if [ ! -f "$cfg/NukeConfig.cmake" ]; then results+=("SKIP  $v (no $cfg/NukeConfig.cmake)"); continue; fi
    bld="$root/${BUILD_PREFIX:-build-}$v"
    [ $clean -eq 1 ] && rm -rf "$bld"
    echo "=== Nuke $v ==="
    cmake "${gen[@]}" -DCMAKE_BUILD_TYPE=Release -DNuke_DIR="$cfg" -S "$root" -B "$bld" \
        || { results+=("FAIL  $v (configure)"); continue; }
    cmake --build "$bld" || { results+=("FAIL  $v (build)"); continue; }
    if [ $install -eq 1 ]; then
        cmake --install "$bld" --prefix "$HOME/.nuke" || { results+=("FAIL  $v (install)"); continue; }
    fi
    results+=("OK    $v")
    built+=("$v")
done

# Stage for scripts\package.ps1 on Windows: linux-bin/<major.minor>/MeshSimplify.so
# plus linux-bin.tar.gz (extract it in the Windows repo root, then run package.ps1).
if [ ${#built[@]} -gt 0 ]; then
    rm -rf "$root/linux-bin"
    for v in "${built[@]}"; do
        mm="${v%%v*}"
        mkdir -p "$root/linux-bin/$mm"
        cp "$root/${BUILD_PREFIX:-build-}$v/MeshSimplify.so" "$root/linux-bin/$mm/"
    done
    tar -czf "$root/linux-bin.tar.gz" -C "$root" linux-bin
fi

echo; echo "==== build_all summary ===="
printf '%s\n' "${results[@]}"
[ ${#built[@]} -gt 0 ] && echo "Staged:  linux-bin/ and linux-bin.tar.gz ($(git -C "$root" rev-parse --short HEAD 2>/dev/null))"
case " ${results[*]} " in *"FAIL "*) exit 1 ;; esac
exit 0
