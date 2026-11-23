#!/usr/bin/env bash
# Release build for Linux: runs scripts/build_all.sh inside a Rocky Linux 8
# container with gcc-toolset-11 (GCC 11, as Foundry uses for Nuke 15-17), so the
# .so files need only glibc 2.28 / the RHEL 8 libstdc++ and load on Rocky 8, 9
# and anything newer. A plain build_all.sh on a new distro (GCC 14, glibc 2.38)
# produces binaries that fail on studio machines with "GLIBC_2.38 not found".
# Toolchain / ABI / farm notes: docs/LINUX_BUILD.md.
#
#   scripts/build_linux_docker.sh [ver ...]        # same version args as build_all.sh
#   NUKE_ROOT=/opt/nukes scripts/build_linux_docker.sh
#   DOCKER=podman scripts/build_linux_docker.sh
#
# Output: linux-bin/<major.minor>/MeshSimplify.so + linux-bin.tar.gz,
# build dirs build-rocky8-<ver>/ (kept apart from the host's build-<ver>/).
# Needs network access once per build dir (CMake FetchContent clones meshoptimizer).
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
nuke_root="$(cd "${NUKE_ROOT:-$HOME/Nuke}" && pwd)"
docker="${DOCKER:-docker}"
image=meshsimplify-rocky8-gcc11

"$docker" build -t "$image" - <<'EOF'
FROM rockylinux:8
RUN dnf -y install gcc-toolset-11-gcc-c++ gcc-toolset-11-binutils cmake make \
        mesa-libGL-devel mesa-libGLU-devel git \
 && dnf clean all
EOF

# Same paths inside as outside, so Nuke_DIR and CMake caches stay valid.
user=(--user "$(id -u):$(id -g)")
[ "$docker" = podman ] && user=(--userns=keep-id)
"$docker" run --rm "${user[@]}" \
    -v "$root:$root:z" -v "$nuke_root:$nuke_root:ro,z" -w "$root" \
    -e NUKE_ROOT="$nuke_root" -e BUILD_PREFIX=build-rocky8- -e HOME=/tmp \
    "$image" bash -c '
        source /opt/rh/gcc-toolset-11/enable
        git config --global --add safe.directory "$PWD" 2>/dev/null || true
        bash scripts/build_all.sh "$@"
        echo
        echo "==== runtime requirements (must be <= GLIBC_2.28 / GLIBCXX_3.4.25) ===="
        for so in linux-bin/*/MeshSimplify.so; do
            g=$(objdump -T "$so" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)
            x=$(objdump -T "$so" | grep -o "GLIBCXX_[0-9.]*" | sort -uV | tail -1)
            echo "$so: $g $x"
        done' _ "$@"
