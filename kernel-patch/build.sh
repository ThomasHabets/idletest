#!/bin/bash
set -euo pipefail
task_dir=$(cd -- "$(dirname -- "$0")" && pwd)
kernel_tree=${1:-"$task_dir/../../linux-idle-zero-rebuild"}
source_archive=${2:-/usr/src/linux-source-6.12.tar.xz}
jobs=${JOBS:-10}
if [[ -e "$kernel_tree" ]]; then
    echo "Refusing to overwrite existing source/build directory: $kernel_tree" >&2
    exit 1
fi
mkdir -p -- "$kernel_tree"
kernel_tree=$(cd -- "$kernel_tree" && pwd)
expected_source_sha=$(awk 'NR == 1 { print $1 }' "$task_dir/source.sha256")
printf '%s  %s\n' "$expected_source_sha" "$source_archive" | sha256sum -c -
tar -xf "$source_archive" -C "$kernel_tree" --strip-components=1
metadata_dir=$(dirname -- "$kernel_tree")/linux-debian-6.12.111-packaging
mkdir -p -- "$metadata_dir"
if [[ ! -f "$metadata_dir/debian.tar.xz" ]]; then
    curl -fL --retry 2 -o "$metadata_dir/debian.tar.xz" \
        https://security.debian.org/debian-security/pool/updates/main/l/linux/linux_6.12.111-1.debian.tar.xz
fi
printf '%s  %s\n' \
    e0146c63a70014f23141fe3e4e830e8a5b2627427d449f3e5415d930016be297 \
    "$metadata_dir/debian.tar.xz" | sha256sum -c -
tar -xf "$metadata_dir/debian.tar.xz" -C "$metadata_dir"
cd -- "$kernel_tree"
while read -r rt_patch; do
    case "$rt_patch" in
        0009-*|001[0-6]-*) patch -p1 < "$metadata_dir/debian/patches-rt/$rt_patch" ;;
    esac
done < "$metadata_dir/debian/patches-rt/series"
patch -p1 < "$task_dir/idle-zero-weight.patch"
cp "$task_dir/kernel.config" .config
make olddefconfig
DEB_BUILD_PROFILES=pkg.linux-upstream.nokerneldbg \
    make -j"$jobs" bindeb-pkg KDEB_PKGVERSION=6.12.111-1 \
        KBUILD_BUILD_VERSION=1 CFLAGS_MODULE=-g0
