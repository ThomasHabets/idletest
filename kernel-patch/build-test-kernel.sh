#!/bin/bash
set -euo pipefail
task_dir=$(cd -- "$(dirname -- "$0")" && pwd)
kernel_tree=${1:-"$task_dir/../../linux-idle-user-test-rebuild"}
source_archive=${2:-/usr/src/linux-source-6.12.tar.xz}
if [[ -e "$kernel_tree" ]]; then
    echo "Refusing to overwrite existing source/build directory: $kernel_tree" >&2
    exit 1
fi
expected_sha=$(awk 'NR == 1 { print $1 }' "$task_dir/source.sha256")
printf '%s  %s\n' "$expected_sha" "$source_archive" | sha256sum -c -
mkdir -p -- "$kernel_tree"
tar -xf "$source_archive" -C "$kernel_tree" --strip-components=1
cd -- "$kernel_tree"
patch -p1 < "$task_dir/idle-zero-weight.patch"
cp "$task_dir/user-test.config" .config
make olddefconfig
make -j"${JOBS:-10}" bzImage KBUILD_BUILD_VERSION=1
cp arch/x86/boot/bzImage "$task_dir/vmlinuz-6.12.111-idle-user-test"
