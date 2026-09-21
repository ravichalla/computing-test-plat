# Test plan

## 1. Purpose

Show, with runnable evidence, that a KVM host and the guests it runs behave as expected, and
that the kernel/user boundary of a small driver is correct. The suite has five parts that
mirror a real validation pipeline: **is the platform ready → do VMs behave → does the driver hold up → can it all run
across a fleet**, plus a **CUDA regression suite** for the GPU software stack.

## 2. Scope

| In scope | Out of scope |
|---|---|
| Host readiness for KVM and PCI passthrough (`platcheck`) | Performance benchmarking |
| VM lifecycle and guest-visible behaviour on x86_64 KVM/QEMU via libvirt (`vmtest`) | Non-x86 guests, Windows guests |
| Kernel char-device contract: bounds, errno, locking (`kmod/valdev`) | Real GPU passthrough and confidential-VM attestation (host readiness is reported, not exercised) |
| Distributed execution: scheduling, retries, routing, failure handling, agent security (`dtest`) | TLS transport, multi-tenant isolation, very large fleets |
| CUDA kernels, Runtime API, Driver API and their error paths (`cudatest`) | Performance benchmarking, multi-GPU / NVLink, tensor-core and cuDNN/cuBLAS library validation, graphics APIs |

## 3. Test environment

- Host: Linux with KVM (`/dev/kvm`), libvirt, QEMU, `qemu-img`. Unprivileged runs work with `qemu:///session`.
- Guest: Ubuntu 24.04 cloud image with `qemu-guest-agent` (built by `scripts/prepare_image.sh`).
- Kernel module tests run **inside a guest**, never on the host.
- Unit tests (no VM needed) run in CI on every push, including under ASan/UBSan; the concurrent scheduler also runs under ThreadSanitizer.
- `dtest` integration tests run agents in-process on loopback ports; the end-to-end script runs the real binaries.

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

### 5.4 Distributed runner — `dtest`

| ID | Area | Test | Expected result |
|---|---|---|---|
| DT-01 | Process runner | stdout / stderr captured separately; exit code and signal reported | exact bytes; `Exited(3)`, `Signaled(9)` |
| DT-02 | Process runner | timeout | process **and its whole process group** are killed; reported `TimedOut` |
| DT-03 | Process runner | background child holds the output pipe after the parent exits | task completes promptly; child is swept |
| DT-04 | Process runner | missing binary vs a script that exits 127 | `LaunchFailed` vs `Exited(127)` (distinguishable) |
| DT-05 | Process runner | output cap, clean environment, stdin is `/dev/null`, concurrent runs | truncation flagged; `HOME` unset; no hang; 8 parallel runs correct |
| DT-06 | Policy | allow list: exact program, directory entry | allowed |
| DT-07 | Policy | `..` traversal, symlink out of an allowed dir, sibling dir sharing a prefix, relative path, empty list | refused |
| DT-08 | Job file | defaults merge, `repeat` expansion, strict validation (22 invalid inputs incl. typo'd keys) | helpful error naming the task |
| DT-09 | Scheduler | slots respected; order preserved; label routing | never more than `slots` at once; results in input order; tasks land only on capable agents |
| DT-10 | Scheduler | retries spend budget; retry prefers another agent; pass-after-fail | `Failed` after N+1 attempts; retried elsewhere; reported `FLAKY` |
| DT-11 | Scheduler | agent outage | agent retired **once** (even if several slots fail together), tasks rescheduled without spending a retry |
| DT-12 | Scheduler | every capable agent gone / no agent / unmatched label | tasks `Unschedulable`; the run terminates (guarded by a watchdog) |
| DT-13 | gRPC | token missing/wrong, agent unreachable | rejected / fails within the connect timeout |
| DT-14 | gRPC | policy violation, missing binary, timeout | task fails (or times out) **without** retiring the agent |
| DT-15 | gRPC | controller lies about capacity | agent answers `RESOURCE_EXHAUSTED` (concurrency limit) |
| DT-16 | gRPC | label routing over the wire; agent stopped mid-run | tasks land on the right agent; run completes on the survivors |
| DT-17 | End to end | real binaries: green run + JUnit, failing task + retry + stderr, wrong token, dead agent, job-file typo, unsafe agent flags | exit codes `0` / `1` / `1` / `1` / `2` / `2` with the expected output |

### 5.5 CUDA regression suite — `cudatest`

| ID | Area | Test | Expected result |
|---|---|---|---|
| CU-01 | Oracle | CPU references, RNG, `allclose` (NaN/Inf rules, tolerance formula, first-mismatch report) | 17 host-only tests pass on any machine |
| CU-02 | Kernels | vector add over 10 sizes around warp/block/grid edges; block-size sweep | bit-identical to the CPU |
| CU-03 | Kernels | out-of-bounds guard: 64 sentinel elements after the output | never overwritten |
| CU-04 | Kernels | 64-bit reduction incl. values that overflow 32 bits; block sweep; repeated launches | exact; no accumulation across launches |
| CU-05 | Kernels | GEMM, 8 shapes incl. 1×N and N×1: naive vs tiled vs CPU (differential) | within rtol/atol 1e-3; tiled is bit-deterministic; A×I = A exactly |
| CU-06 | Kernels | histogram: random, single-bin worst-case contention, non-power-of-two and 1-thread blocks | exact bin counts |
| CU-07 | Kernels | transpose, 8 shapes incl. 1×100, 257×131; double transpose | exact; involution holds |
| CU-08 | Launch validation | bad block size (0, 2048, non-power-of-two), negative sizes, null pointers | `cudaErrorInvalidConfiguration` / `InvalidValue`; later launches unaffected; no sticky error |
| CU-09 | Runtime API | properties, versions, `cudaMemGetInfo`, memcpy 1 B–16 MiB, memset, D2D, pinned + async, events, unified memory | sane values; exact round trips |
| CU-10 | Runtime API | cross-stream ordering with `cudaStreamWaitEvent`; 8 host threads sharing the device | dependent result correct; all threads correct |
| CU-11 | Runtime errors | impossible allocation, double free, host-pointer free, null memcpy, invalid device ordinal | expected error code; last-error recorded then cleared; device still usable |
| CU-12 | Driver API | init, primary context, alloc/copy, error names, device identity matches the Runtime API | consistent |
| CU-13 | Driver API | PTX module load + `cuLaunchKernel`; missing kernel name; garbage image; zero-byte alloc; bogus free | correct result; `NOT_FOUND`; load fails; `INVALID_VALUE` |
| CU-14 | Interop | `cudaMalloc` pointer used with `cuMemcpyDtoH` | works (shared primary context) |
| CU-15 | Build | `ptxas` assembles the PTX file; kernels build for sm_75–90 with PTX | no errors |

## 6. Entry and exit criteria

- **Entry:** `platcheck` reports no `Fail` on the target host; base image boots and its guest agent answers.
- **Exit (a run is green when):** all unit and integration tests pass (CI); `vmtest` exits 0 (skips allowed);
  `test_valdev` reports 15/15; `cudatest_gpu_tests` passes on a GPU host with `CUDATEST_REQUIRE_GPU=1`; sanitizer builds are clean; `dtest-ctl` exits 0 on the fleet job.

## 7. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Boot time dominates run time | Guest-behaviour tests share one VM; `--filter` selects subsets |
| Flaky readiness (agent slow to start) | Poll with a configurable timeout (`--boot-timeout`) instead of fixed sleeps |
| Race conditions are probabilistic | Concurrency tests are tunable (`VALDEV_ITERS`) and must run on a multi-core guest |
| Kernel module could destabilize a machine | `run_in_vm.sh` refuses to run on bare metal without `--force` |
| Leaked VMs / disks after a crash | Transient domains with `AUTODESTROY`; overlays removed in teardown |
| A test failure is mistaken for a broken host (or vice versa) | Failed tests spend retries; infrastructure errors retire the agent and reschedule for free |
| Agent used as a remote-code-execution foothold | Refuses to start without auth and an allow list; canonicalised paths; plaintext transport documented as trusted-network only |
| Scheduler deadlock under odd failure combinations | Termination rules covered by tests; every scheduler test runs under a watchdog |
| Orphaned processes after a timeout | Own process group, SIGTERM then SIGKILL sweep before reaping |
| GPU tests silently skipped on a broken GPU runner | `CUDATEST_REQUIRE_GPU=1` makes "no usable GPU" a failure |
| Float comparisons that are too strict or too loose | Exact where the maths is exact; explicit rtol/atol for GEMM; the comparison helper is itself unit tested |
| A sticky CUDA error poisons every later test | Invalid launches are caught by argument validation; error-path tests never trigger a sticky error such as an illegal memory access |

## 8. Known gaps / next steps

- Add a guest-side CPU/memory stress workload and check for guest kernel oops in the console log.
- Exercise VFIO GPU passthrough when a passthrough-capable host is available.
- Run `test_valdev` automatically inside a `vmtest` guest (guest-exec the build, then collect TAP).
- Track pass rate and boot-time trend across runs.
- `cudatest`: multi-GPU and peer-to-peer copies, cuBLAS cross-checks for GEMM, compute-sanitizer runs in CI, performance-regression thresholds.
- `dtest`: TLS or mTLS, streaming output for long tasks, controller-side result persistence, and a multi-host soak.
