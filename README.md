# computing-test-plat

# kvm-validation-suite

A small, test-driven toolkit for validating a Linux virtualization stack end to end:

| Component | Language | What it does |
|---|---|---|
| [`platcheck`](platcheck/) | C++17 | Checks whether a host is ready for KVM and GPU passthrough (CPU flags, `/dev/kvm`, nested virt, IOMMU groups, VFIO, hugepages, NVIDIA GPU group isolation, SEV/TDX indicators). Text or JSON output; exit code is CI-friendly. |
| [`vmtest`](vmtest/) | C++17, libvirt C API | Boots a KVM guest, drives it through the QEMU guest agent, and validates lifecycle and guest-visible behaviour. Emits JUnit XML. |
| [`kmod/`](kmod/) | C (Linux kernel) | `valdev`, a small character device, plus a TAP test suite for its kernel/user contract: bounds, errno values, `EFAULT` handling and locking under concurrency. |

The written strategy, test matrix, entry/exit criteria and risks are in [`docs/TEST_PLAN.md`](docs/TEST_PLAN.md).

## Build and run the unit tests

```bash
sudo apt-get install -y build-essential cmake pkg-config libvirt-dev libgtest-dev nlohmann-json3-dev
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure      # 47 tests, no VM or hypervisor needed
```

## 1. platcheck: is this host ready?

```bash
build/platcheck                 # human-readable
build/platcheck --json          # machine-readable
build/platcheck --groups        # also dump IOMMU groups and their devices
```

Exit status: `0` no failed checks, `1` at least one `FAIL`, `2` usage error. Warnings and skips never fail a run.

Example, from a VM that has no KVM (so the failures are expected):

```
[FAIL] /dev/kvm accessible: /dev/kvm does not exist
       -> Load the kvm module (modprobe kvm_intel / kvm_amd) and check firmware settings.
[FAIL] IOMMU enabled: no IOMMU groups found; kernel cmdline has no intel_iommu/amd_iommu option
       -> Enable VT-d/AMD-Vi in firmware and boot with intel_iommu=on (or amd_iommu=on) and optionally iommu=pt.
[SKIP] NVIDIA GPU IOMMU isolation: no NVIDIA display device found in any IOMMU group
```

Every check reads through a configurable root (`--root DIR`), which is how the unit tests drive each branch
against a fake `/proc` and `/sys` tree.

## 2. vmtest: do VMs behave?

Prepare a base image once (installs `qemu-guest-agent` into an Ubuntu 24.04 cloud image):

```bash
scripts/prepare_image.sh vmtest-base.qcow2
```

Run the suite:

```bash
build/vmtest --image vmtest-base.qcow2 --junit results.xml
build/vmtest --image vmtest-base.qcow2 --filter guest.          # subset
build/vmtest --image vmtest-base.qcow2 --iterations 20          # add the boot/destroy soak test
build/vmtest --list                                             # list test ids
```

Needs KVM, libvirt and `qemu-img` on the host. For an unprivileged run use `--uri qemu:///session`;
with `qemu:///system` the work directory must be readable by the QEMU user (`--work-dir`).

Design points:
- **Isolation:** every guest boots from a throwaway qcow2 overlay; the base image is never modified.
- **No leaks:** domains are transient and created with `VIR_DOMAIN_START_AUTODESTROY`, so a crashed run cannot leave a VM behind.
- **No fixed sleeps:** readiness is polled with a timeout (`--boot-timeout`).
- **Shared guest:** the `guest.*` tests reuse one VM so boot cost is paid once; `--filter` still works because a VM is booted on demand.

Exit status: `0` all passed (skips allowed), `1` a test failed, `2` usage error.

## 3. kmod/valdev: does the driver hold up?

`valdev` exposes a 4 KiB buffer (read/write/llseek) and a 64-bit counter (ioctl). The test program prints TAP.

```bash
cd kmod
make                        # builds valdev.ko (needs linux-headers) and test_valdev
./run_in_vm.sh              # insmod, run the tests, rmmod; refuses to run on bare metal
VALDEV_ITERS=200000 ./test_valdev   # heavier concurrency soak
```

Load it only inside a throwaway VM. `run_in_vm.sh` checks `systemd-detect-virt` and requires `--force` on bare metal.
Race detection is probabilistic and needs more than one CPU.

## Engineering notes

- **A bug the tests caught in the driver design.** The first `write()` copied user data straight into the shared
  buffer. `copy_from_user()` zero-fills the uncopied tail when it faults, so a single bad pointer would wipe existing
  data. The write path now copies into a bounce buffer first (`memdup_user`) and only then takes the lock and
  copies in. `bad_user_pointers_are_efault` asserts the data is still intact afterwards.
- **Locks are not held across user copies.** Reads snapshot under the mutex and call `copy_to_user()` after
  dropping it, so a page fault cannot stall other users of the device.
- **`-Werror` and fortify caught a test bug.** A test read up to 100 bytes into a 16-byte buffer; the compiler flagged it.
- **CI** runs the unit tests, an ASan/UBSan build, and a compile check of the kernel module (`.github/workflows/ci.yml`).

## Status

| Piece | Verified how |
|---|---|
| `platcheck` | 24 unit tests against a fake sysfs tree; clean under ASan/UBSan; run on a real host |
| `vmtest` logic | 23 unit tests (parsing, JUnit, domain XML, runner classification); clean under ASan/UBSan; failure paths run against a host with no libvirt daemon |
| `vmtest` against a real guest | **Not run in the environment this was written in** (no KVM available). Run it on a KVM host and expect to tweak timeouts or image details. |
| `valdev.ko` | Compiles warning-free with `W=1` against Linux 6.8 headers. **Not loaded** in the environment this was written in. |
| `test_valdev` | Built with `-Wall -Wextra -Werror`; 15/15 against a userspace model of the device, and it fails when that model returns the wrong errno. Not yet run against the real module. |

## Layout

```
platcheck/   host readiness library, CLI and unit tests
vmtest/      libvirt harness: framework, guest-agent client, JUnit writer, tests, unit tests
kmod/        valdev.c, shared ioctl ABI header, TAP test program, run_in_vm.sh
docs/        TEST_PLAN.md
scripts/     prepare_image.sh
```
