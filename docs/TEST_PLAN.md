# Test plan

## 1. Purpose

Show, with runnable evidence, that a KVM host and the guests it runs behave as expected, and
that the kernel/user boundary of a small driver is correct. The suite has three parts that
mirror a real validation pipeline: **is the platform ready → do VMs behave → does the driver hold up**.

## 2. Scope

| In scope | Out of scope |
|---|---|
| Host readiness for KVM and PCI passthrough (`platcheck`) | Performance benchmarking |
| VM lifecycle and guest-visible behaviour on x86_64 KVM/QEMU via libvirt (`vmtest`) | Non-x86 guests, Windows guests |
| Kernel char-device contract: bounds, errno, locking (`kmod/valdev`) | Real GPU passthrough and confidential-VM attestation (host readiness is reported, not exercised) |

## 3. Test environment

- Host: Linux with KVM (`/dev/kvm`), libvirt, QEMU, `qemu-img`. Unprivileged runs work with `qemu:///session`.
- Guest: Ubuntu 24.04 cloud image with `qemu-guest-agent` (built by `scripts/prepare_image.sh`).
- Kernel module tests run **inside a guest**, never on the host.
- Unit tests (no VM needed) run in CI on every push, including under ASan/UBSan.

## 4. Approach

1. **Test the tests.** Harness logic (argument parsing, JUnit output, agent-reply parsing, runner
   classification) is unit tested without a hypervisor. Host checks run against a fake `/proc` + `/sys`
   tree so every Pass/Warn/Fail/Skip branch is exercised deterministically.
2. **Isolate every VM.** Each guest boots from a throwaway qcow2 overlay; the base image is never written.
   Domains are transient and auto-destroyed when the connection closes, so an aborted run leaves nothing behind.
3. **Distinguish failure from not-applicable.** `Skip` is a first-class outcome (e.g. no SEV/TDX on this CPU);
   only `Fail` changes the exit code.
4. **Machine-readable results.** `platcheck --json`, JUnit XML from `vmtest`, and TAP from `test_valdev`.

## 5. Test matrix

### 5.1 Host readiness — `platcheck`

| ID | Check | Pass | Warn | Fail |
|---|---|---|---|---|
| PC-01 | CPU virtualization flags | `vmx` or `svm` present | – | neither present |
| PC-02 | `/dev/kvm` | exists, R/W for user | exists, no access | missing |
| PC-03 | KVM module | `kvm_intel` / `kvm_amd` / `kvm` loaded | – | none loaded |
| PC-04 | Nested virtualization | parameter enabled | parameter disabled | – |
| PC-05 | IOMMU | IOMMU groups present | – | no groups |
| PC-06 | VFIO modules | `vfio`, `vfio_pci`, `vfio_iommu_type1` | any missing | – |
| PC-07 | Hugepages | `HugePages_Total > 0` | none reserved | – |
| PC-08 | NVIDIA GPU IOMMU isolation | GPU group holds only GPU + its audio fn + bridges | group shared with other endpoints | – |
| PC-09 | Confidential computing (SEV/SEV-ES/SEV-SNP/TDX) | indicator enabled | – | – (`Skip` if absent) |

### 5.2 Guest behaviour — `vmtest`

| ID | Test | Expected result |
|---|---|---|
| VT-01 | `lifecycle.boot_and_agent_ping` | Domain reaches *running*; guest agent answers; libvirt reports configured vCPUs |
| VT-02 | `lifecycle.pause_resume` | *paused* then *running*; agent reachable ≤ 30 s after resume |
| VT-03 | `guest.vcpu_count_matches` | `nproc` in guest equals configured vCPUs |
| VT-04 | `guest.memory_matches` | Guest `MemTotal` is between 70 % and 100 % of configured RAM |
| VT-05 | `guest.hypervisor_cpu_flag` | Guest CPU advertises `hypervisor` |
| VT-06 | `guest.exit_code_propagates` | Exit status 7 in guest is reported as 7 |
| VT-07 | `guest.stdout_stderr_capture` | stdout and stderr captured separately and exactly |
| VT-08 | `lifecycle.graceful_shutdown` | Guest powers off within the timeout; domain disappears |
| VT-09 | `stress.boot_destroy_soak` | N boot/destroy cycles with no leaked domains or overlays (opt-in via `--iterations`) |

### 5.3 Driver contract — `kmod/valdev`

| ID | Test | Expected result |
|---|---|---|
| KD-01 | open/close | succeeds |
| KD-02 | ABI version ioctl | equals `VALDEV_ABI_VERSION` |
| KD-03 | write/read round trip | bytes identical |
| KD-04 | read of empty buffer | returns 0 (EOF) |
| KD-05 | partial reads follow file offset | reads stop at end of valid data |
| KD-06 | write straddling capacity | short write, then `ENOSPC` |
| KD-07 | write at capacity | `ENOSPC` |
| KD-08 | llseek validation | `EINVAL` for negative / past-capacity; `SEEK_END`/`SEEK_CUR` correct |
| KD-09 | counter accumulates | returns running total |
| KD-10 | reset | clears counter and buffer |
| KD-11 | unknown ioctl | `ENOTTY` |
| KD-12 | bad user pointers | `EFAULT` on every path **and existing data is not corrupted** |
| KD-13 | open statistics | opens counter increments by one per `open()` |
| KD-14 | concurrent counter | no lost updates (8 threads × N increments) |
| KD-15 | concurrent writers | disjoint regions never corrupt each other |

## 6. Entry and exit criteria

- **Entry:** `platcheck` reports no `Fail` on the target host; base image boots and its guest agent answers.
- **Exit (a run is green when):** all unit tests pass (CI); `vmtest` exits 0 (skips allowed); `test_valdev` reports
  15/15; sanitizer build is clean.

## 7. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Boot time dominates run time | Guest-behaviour tests share one VM; `--filter` selects subsets |
| Flaky readiness (agent slow to start) | Poll with a configurable timeout (`--boot-timeout`) instead of fixed sleeps |
| Race conditions are probabilistic | Concurrency tests are tunable (`VALDEV_ITERS`) and must run on a multi-core guest |
| Kernel module could destabilize a machine | `run_in_vm.sh` refuses to run on bare metal without `--force` |
| Leaked VMs / disks after a crash | Transient domains with `AUTODESTROY`; overlays removed in teardown |

## 8. Known gaps / next steps

- Add a guest-side CPU/memory stress workload and check for guest kernel oops in the console log.
- Exercise VFIO GPU passthrough when a passthrough-capable host is available.
- Run `test_valdev` automatically inside a `vmtest` guest (guest-exec the build, then collect TAP).
- Track pass rate and boot-time trend across runs.
