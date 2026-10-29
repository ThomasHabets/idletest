This experimental patch targets Linux 6.12.111 and adds the runtime
scheduler feature `IDLE_ZERO_WEIGHT`, disabled by default. It gives idle
userspace work no CPU service while other fair work is runnable on the
same CPU, while preserving ordinary positive-weight service for kernel
execution.

The patch has changed since the original build. The existing files under
`packages/` and `vmlinuz-6.12.111-idle-zero` contain the OLD implementation,
which can indefinitely starve kernel lock holders. Rebuild before using
this revision. The full build now uses release `6.12.111-idle-user` and
package version `6.12.111-2` to distinguish it from those older binaries.
The updated patch has been tested in a separate, small RT VM kernel;
updated host-installable Debian packages have not been built or installed.

After booting a kernel built with the updated patch, enable the feature:

```sh
echo IDLE_ZERO_WEIGHT | sudo tee /sys/kernel/debug/sched/features
```

Restore ordinary weighted scheduling:

```sh
echo NO_IDLE_ZERO_WEIGHT | sudo tee /sys/kernel/debug/sched/features
```

Debugfs must be mounted at `/sys/kernel/debug`. The toggle is system-wide,
resets on reboot, and takes effect at subsequent scheduling decisions.

A per-task marker is cleared on generic entry from userspace, including
syscalls, interrupts and faults. It is set only at the final, lock-free
return-to-userspace gate after pending syscall and return work has been
processed. The gate can reschedule an idle task before any further user
instructions execute. If the task resumes, kernel eligibility is restored
before processing newly queued work. Architecture-specific finalization
runs only after the last such reschedule.

Saved user registers alone cannot identify kernel execution: a syscall
retains user-mode registers even while the task holds kernel locks.
Kernel threads, tasks executing kernel code, pending signals/return work,
and delayed dequeues are exempt from strict starvation. Architectures
without generic-entry instrumentation conservatively retain ordinary
weighted service. Kernel work keeps its existing positive fair weight;
this is not a real-time priority boost or a bounded-latency guarantee.

The rule covers SCHED_IDLE tasks, idle-only descendants of CPU cgroups,
and groups marked `cpu.idle=1`. Classification walks actual descendant
states without recursive C calls, so a kernel-mode task can make progress
even inside an otherwise idle group. It does not rely on policy-only idle
counts. Higher scheduling classes, the fair server, affinity restrictions
and cgroup CPU quotas remain separate controls.

Positive internal arithmetic weights are preserved. Selection and the
virtual-time average exclude only idle userspace entities during mixed
contention. Waiting idle userspace entities advance with normal virtual
time without receiving CPU service, avoiding unbounded debt and catch-up
bursts. Kernel execution may therefore preempt normal userspace work.

This does not make arbitrary starvation deadlock-free. A task stopped in
userspace can still own a userspace mutex or another resource needed by a
normal task. Strict starvation can still prevent that owner from releasing
it. The exemption addresses in-flight kernel execution, not all possible
priority inversions or locks whose ownership persists into userspace.

The implementation scans runqueues and, for groups, descendant entities.
Rebasing also performs tree maintenance. This is experimental and has not
been performance-qualified for large hierarchies or production RT latency.
Entry/return tracking and the return gate add scheduling-path overhead.

`build.sh` creates a separate kernel source/build tree, verifies the source
archive checksum, and applies the patch. It uses Debian's Linux 6.12.111-1
source archive, defaulting to `/usr/src/linux-source-6.12.tar.xz`, and the
recorded `kernel.config`. Its first argument overrides the destination
and its second overrides the archive; `JOBS` defaults to 10.

The full build uses static PREEMPT_RT plus Debian's eight official i915
RT-support fixes (patches 0009 through 0016 from its separate RT series).
The script downloads and verifies Debian's unmodified packaging archive
beside the kernel tree, outside this project. Debian's optional dynamic/
lazy preemption extensions are excluded. `config.diff` records the other
configuration differences. BTF and module DWARF information are disabled,
modules use locally generated signing keys, and the kernel image is not
signed for Secure Boot. Building produces image and headers packages
beside the source tree without installing them.

Build requirements include gcc, make, bc, bison, flex, libelf-dev,
libssl-dev, cpio, kmod, rsync, debhelper, curl, patch and sha256sum.
The debug-symbol package is skipped.

For a smaller validation build, run:

```sh
./build-test-kernel.sh
./test-vm.sh
```

`build-test-kernel.sh` verifies and extracts the same source archive in a
fresh separate tree, applies the scheduler patch, and uses the recorded
`user-test.config`. It produces the ignored test image
`vmlinuz-6.12.111-idle-user-test`. This small configuration supports QEMU,
PREEMPT_RT, CPU cgroups and kernel-mode userfaultfd, rather than the full
host driver set. `test-vm.sh [path/to/bzImage]` accepts another image too.
It uses KVM when available, otherwise TCG, and requires QEMU, gcc and a
static C library. The VM-only PID-1 test program must not run on the host.
QEMU's ISA debug-exit device terminates completed tests independently of
ACPI poweroff support.

The suite checks weighted progress with the feature disabled, zero idle
userspace iterations under sustained contention, nice-19 normal work,
normal cgroup weight 1 versus idle weight 10000, normal sleep/resume,
runtime toggling, sharing between two normal workers, nested CPU cgroups,
and `cpu.idle` groups. Pending SIGKILL must be serviced while the normal
burner remains runnable.

The lock regression stalls `pipe_write()` in `copy_page_from_iter()` with
userfaultfd while it owns `pipe->mutex`. A normal reader blocks on that
mutex, and a second normal task spins continuously on the same CPU. After
the fault is resolved, the idle writer must finish kernel work and release
the mutex, allowing the reader to complete. Its first userspace store after
write() must remain blocked until foreground work stops. This is exercised
for direct SCHED_IDLE, nested groups, and a normal-policy writer inside a
`cpu.idle` group. Run only these cases with:

```sh
VM_LOCK_ONLY=1 ./test-vm.sh /path/to/kernel
```

Validation: the previous kernel fails all three mutex-progress checks;
the updated RT test kernel passes all 30 checks, including pending SIGKILL.
The patch passes `git apply --check` against upstream v6.12.111 commit
`e2acc2211022246c77740d5df08265cc27eedcc5` as well as the Debian source.
The final test-kernel build has no compiler/objtool warnings, and the guest
has no kernel warnings or oopses. The low-share cpu.idle baseline uses a
ten-second window; strict-starvation checks use two-second windows after
settling. Tests leave the guest fair-server reservation at its defaults
and do not modify host scheduling settings.

`vm-test.log` contains the latest VM output. `vm-test-old-lock.log` records
the reproduced failures in the old kernel. These logs and all binary
outputs are ignored by Git. `source.sha256` records the original input
archives. `SHA256SUMS` now covers the current source/configuration inputs
and the newly validated test image, not the old host packages.
