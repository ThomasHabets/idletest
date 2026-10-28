#!/bin/bash
set -euo pipefail
task_dir=$(cd -- "$(dirname -- "$0")" && pwd)
kernel_image=${1:-/home/marvin/scm/linux-idle-zero/arch/x86/boot/bzImage}
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
gcc -O2 -Wall -Wextra -Werror -static -o "$stage/init" "$task_dir/vm-test.c"
(
    cd "$stage"
    printf '%s\n' init | cpio --quiet -o -H newc | gzip -1
) > "$task_dir/test-initramfs.cpio.gz"
accelerator=tcg
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    accelerator=kvm
fi
timeout 240 qemu-system-x86_64 \
    -accel "$accelerator" -cpu max -smp 2 -m 1024 \
    -kernel "$kernel_image" -initrd "$task_dir/test-initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=-1 quiet' \
    -nographic -monitor none -no-reboot \
    > "$task_dir/vm-test.log" 2>&1
if ! rg -q 'IDLE_ZERO_WEIGHT_TEST_RESULT: PASS' "$task_dir/vm-test.log"; then
    cat "$task_dir/vm-test.log"
    exit 1
fi
rg 'PASS:|FAIL:|IDLE_ZERO_WEIGHT_TEST_RESULT' "$task_dir/vm-test.log"
