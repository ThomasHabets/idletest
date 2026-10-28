This is an experimental kernel based on Debian's Linux 6.12.111-1 source
archive with static PREEMPT_RT and Debian's i915 RT-support fixes (patches
0009 through 0016 from its separate RT series). The source/build tree is
`/home/marvin/scm/linux-idle-zero`.
The kernel release is `6.12.111-idle-zero`. Nothing in the host's boot
configuration is changed by building or testing it.

This run's Debian image and headers packages are collected in `packages/`.
The image package contains the kernel and its matching modules. The headers
package supports building external modules for this kernel.

The patch adds the runtime scheduler feature `IDLE_ZERO_WEIGHT`, disabled
by default. After booting the patched kernel, enable it with:

```sh
echo IDLE_ZERO_WEIGHT | sudo tee /sys/kernel/debug/sched/features
```

Restore ordinary scheduling with:

```sh
echo NO_IDLE_ZERO_WEIGHT | sudo tee /sys/kernel/debug/sched/features
```

Debugfs must be mounted at `/sys/kernel/debug`. This toggle is system-wide
and resets on reboot. It takes effect at subsequent scheduler decisions;
it does not provide a bound on interrupt or preemption latency.

With the feature enabled, non-idle fair work has strict precedence over
idle work queued on the same CPU. This includes SCHED_OTHER and SCHED_BATCH
versus SCHED_IDLE, idle-only descendants of ordinary CPU cgroups, and
groups marked `cpu.idle=1` versus ordinary sibling groups. Tasks on other
CPUs, and normal tasks that are blocked or cgroup-throttled, do not prevent
idle work from running on an available CPU.

The implementation preserves positive internal weights. It excludes idle
entities from selection and the virtual-time average when normal and idle
work compete, so deferred idle work cannot pull new normal tasks backwards
in virtual time. Waiting idle entities advance with normal virtual time
without receiving service, avoiding unbounded debt and catch-up bursts.
Ordinary EEVDF remains in use when no such competition
exists. Wakeup preemption and the next-buddy shortcut respect the feature.
The fair server and FIFO/RR settings are independent and need no changes
for strict SCHED_OTHER-versus-SCHED_IDLE precedence.

This implementation scans a mixed runqueue in O(n), including when
calculating its virtual-time average; rebasing idle entities can require
O(n log n) tree maintenance. It is intended for experimentation
and has not been performance-qualified for large runqueues or production
real-time latency requirements. It can indefinitely starve background work.
The build disables BTF and module DWARF debug information and uses locally generated module
signing keys; the kernel image is not signed for Secure Boot.
Debian's optional dynamic/lazy preemption extensions are not included.
`config.diff` lists the differences from the running Debian kernel's
configuration. In particular, PREEMPT_RT and the machine's i915 driver are
retained; dynamic/lazy preemption and BTF-dependent tracing are unavailable.

`idle-zero-weight.patch` contains only the scheduler changes.
`kernel.config` records the build configuration. `build.sh` reproduces the
build in a fresh, separate directory using `/usr/src/linux-source-6.12.tar.xz`;
it must be the Debian 6.12.111-1 source archive. Its first argument overrides
the destination directory and its second overrides the archive. `JOBS`
defaults to 10. The build requires the usual kernel build tools and Debian
packaging tools (gcc, make, bc, bison, flex, libelf-dev, libssl-dev, cpio,
kmod, rsync and debhelper). It produces Debian image and headers packages
beside the source tree without installing them. The debug-symbol package
is skipped.
The script downloads and verifies Debian's packaging archive to obtain the
eight official i915 RT fixes. This unmodified archive is kept beside the
kernel tree, outside this example project. curl, patch and sha256sum are
also required. The archive SHA-256 is pinned in the script.

`test-vm.sh [path/to/bzImage]` builds a small static initramfs and boots the
kernel in QEMU with two CPUs. It uses KVM if available, otherwise TCG.
`vm-test.c` is a VM-only PID-1 test program. It checks that:

- Disabling the feature permits both normal and idle work to progress.
- Enabling it gives idle work zero measured iterations under sustained
  normal contention, including when the normal worker has nice value 19.
- Blocking the normal worker lets idle work progress; resuming it suppresses
  idle work again.
- Toggling the feature back restores weighted sharing.
- Two normal workers continue sharing the CPU while an idle worker starves.
- The rule also applies across nested CPU cgroups and `cpu.idle` groups.
- Normal work still takes precedence when its cgroup has weight 1 and the
  competing idle-only cgroup has weight 10000.

`vm-test.log` records the guest output. The test leaves the guest fair-server
reservation at its default value. The host's scheduling settings are not
modified.

Validation of the built image: all 20 VM checks passed, with no guest kernel
warnings or oopses. The low-weight `cpu.idle` baseline uses a ten-second
observation window; strict-starvation checks use two-second windows after
a settling interval. Patch style checking reported no code-style errors
or warnings, and reverse-application checking confirmed the patch matches
the build tree. `SHA256SUMS` identifies the image, configuration, patch and
test/build scripts. `source.sha256` identifies the original source archive.
The image extracted from the Debian package matches the VM-tested image
byte for byte. The package includes i915, whose module version identifies
`6.12.111-idle-zero` with `preempt_rt`. The image package is about 85 MiB;
the headers package is about 9 MiB. These packages have not been installed.
