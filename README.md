# kvm-validation-suite

A small, test-driven toolkit for validating a Linux virtualization stack end to end, from "is this host
ready?" to "do the guests behave?" to "does the driver hold up?", plus a distributed runner that fans the
work out across a fleet of hosts.

| Component | Language | What it does |
|---|---|---|
| [`platcheck`](platcheck/) | C++17 | Checks whether a host is ready for KVM and GPU passthrough (CPU flags, `/dev/kvm`, nested virt, IOMMU groups, VFIO, hugepages, NVIDIA GPU group isolation, SEV/TDX indicators). Text or JSON output; CI-friendly exit codes. |
| [`vmtest`](vmtest/) | C++17, libvirt C API | Boots a KVM guest, drives it through the QEMU guest agent, and validates lifecycle and guest-visible behaviour. Emits JUnit XML. |
| [`kmod/`](kmod/) | C (Linux kernel) | `valdev`, a small character device, plus a TAP test suite for its kernel/user contract: bounds, errno values, `EFAULT` handling and locking under concurrency. |
| [`cudatest/`](cudatest/) | C++17, CUDA C++ | A CUDA regression suite: five kernels checked against CPU references (and against each other), Runtime API and Driver API behaviour, PTX module loading, and negative/error-path tests. Skips cleanly without a GPU. |
| [`dtest/`](dtest/) | C++17, gRPC | A distributed test runner: an **agent** on every host runs commands, a **controller** schedules a job file across agents with label routing, retries, flaky-test detection and infrastructure-failure handling. Emits JUnit XML. |

The written strategy, test matrix, entry/exit criteria and risks are in [`docs/TEST_PLAN.md`](docs/TEST_PLAN.md).

## Build and run the unit tests

```bash
sudo apt-get install -y build-essential cmake pkg-config libvirt-dev libgtest-dev nlohmann-json3-dev \
    libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure      # 121 tests + a 6-scenario end-to-end script, no special hardware
```

No VM, hypervisor, KVM or GPU is needed for any of that. If gRPC is not installed, CMake still builds everything
except the dtest agent and controller and says so. Use `-DBUILD_VMTEST=OFF` / `-DBUILD_DTEST=OFF` / `-DBUILD_CUDATEST=OFF`
to skip a component.

For the CUDA suite, additionally install the CUDA toolkit (`sudo apt-get install nvidia-cuda-toolkit`, or NVIDIA's own
packages). Without it CMake still builds and runs the host-only part of `cudatest` and tells you so. With the toolkit, the
kernels and GPU tests are built too; 89 of those tests need an actual NVIDIA GPU and are skipped without one.
Ubuntu 24.04's nvcc 12.0 rejects GCC 13, so the build automatically uses `g++-12` for CUDA when it is installed
(`sudo apt-get install g++-12`).

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

## 4. dtest: run it across a fleet

```
                         +-------------------+
   jobs.json  ---------> |    dtest-ctl      |  scheduler: label routing, retries,
                         |   (controller)    |  agent-failure handling, JUnit report
                         +---------+---------+
                                   | gRPC (bearer token)
              +--------------------+--------------------+
              v                                         v
     +-------------------+                     +------------------+
     |  dtest-agent      |                     |  dtest-agent     |
     |  os=linux kvm=true|                     |  os=linux        |
     |  slots=4          |                     |  slots=8         |
     +-------------------+                     +------------------+
      runs commands in their own process group; enforces timeouts, output caps, an allow list
```

Start an agent on each test host, then run a job file from anywhere:

```bash
# on each host (kvm=true is detected automatically from /dev/kvm; add your own labels)
export DTEST_TOKEN=change-me
dtest-agent --listen 10.0.0.5:7001 --allow /opt/kvm-validation-suite/bin/ --label rack=r2

# from the controller
export DTEST_TOKEN=change-me
dtest-ctl --jobs dtest/examples/jobs.json --agent 10.0.0.5:7001 --agent 10.0.0.6:7001 --junit results.xml
```

A job file (`dtest/examples/jobs.json` runs `platcheck` and `vmtest` this way; `dtest/examples/smoke.json` runs anywhere):

```json
{
  "name": "smoke",
  "defaults": { "timeout_s": 10, "retries": 1 },
  "tasks": [
    { "name": "hello",       "argv": ["/bin/sh", "-c", "echo hello from $(hostname)"] },
    { "name": "burst",       "argv": ["/bin/sh", "-c", "sleep 0.2"], "repeat": 6 },
    { "name": "hangs",       "argv": ["/bin/sh", "-c", "sleep 60"], "timeout_s": 1, "retries": 0 },
    { "name": "needs-a-gpu", "argv": ["/bin/true"], "requires": ["gpu=true"] }
  ]
}
```

Output from a real run of `smoke.json` against one local agent (it also has an always-failing task):

```
[PASS] hello on 127.0.0.1:7001 (0.0s)
[PASS] burst[1] on 127.0.0.1:7001 (0.2s)
...
[FAIL] exit-code on 127.0.0.1:7001 (0.0s, 2 attempts) - exit code 1
        | assertion failed: expected 4, got 5
[TIMEOUT] hangs on 127.0.0.1:7001 (1.0s) - timed out (exceeded timeout of 1000 ms)
[SKIPPED] needs-a-gpu - no agent provides the required labels [gpu=true]

10 tasks: 7 passed, 1 failed, 1 timed out, 1 unschedulable in 1.6s
```

### Scheduling rules

- A task runs only on agents whose labels satisfy its `requires` (`"kvm=true"` exact, or `"kvm"` for any value).
- Each agent runs at most `slots` tasks at once; the agent enforces this too, so a buggy controller cannot overload it.
- **A failed test is not the same as a failed machine.** A non-zero exit or a timeout spends the task's `retries`,
  and the retry prefers a *different* agent. A task that passes after failing is reported `FLAKY` (still green, but visible).
- An infrastructure failure (agent unreachable, bad token) retires that agent and reschedules its tasks **without**
  spending a retry. A task nobody can run is reported `SKIPPED` (unschedulable) instead of hanging the run.
- Job files are validated strictly: unknown keys such as `"retires"` are errors, not silent no-ops.

Exit status of `dtest-ctl`: `0` all tasks passed, `1` any task failed, timed out or was unschedulable, `2` usage or job-file error.

### Security model

An agent is remote code execution by design, so it is locked down by default:

- It **refuses to start** without both an authentication choice (`--token` / `DTEST_TOKEN`, or an explicit `--no-auth`)
  and a command policy (`--allow PATH`, repeatable, or an explicit `--allow-any`).
- The allow list is checked against **canonicalised** paths, so `allowed/../../bin/sh` and a symlink inside an allowed
  directory pointing outside it are both refused. Commands must be absolute paths.
- Tokens are compared in constant time; commands run with a minimal clean environment.
- Anything you allow, the controller can run. Allowing `/bin/sh` allows arbitrary shell, so prefer allowing a directory of
  purpose-built test binaries.
- **Transport is plaintext gRPC.** Bind to loopback, or run on a trusted network / through a tunnel (VPN, SSH port forward).
  The agent warns when it binds to a non-loopback address. TLS is not implemented.

## 5. cudatest: CUDA regression suite

```bash
cmake -S . -B build && cmake --build build -j
build/cudatest_reference_tests        # host-only: the oracle itself; runs anywhere
build/cudatest_gpu_tests              # needs an NVIDIA GPU; skips (exit 0) without one
CUDATEST_REQUIRE_GPU=1 build/cudatest_gpu_tests    # on a GPU runner: "no GPU" is a failure, not a skip
```

What it checks:

| Area | Tests |
|---|---|
| Kernels (`cudatest/src/kernels.cu`) | vector add, 64-bit reduction, GEMM (naive **and** shared-memory tiled), 256-bin histogram with shared-memory privatisation, padded-tile transpose. Sizes straddle warp (32), block (256) and grid boundaries; block sizes are swept; results are compared with a CPU reference (exact where the maths is exact, tolerance-based for float GEMM). Also: nothing is written past the end of an output, repeated launches do not accumulate, results are deterministic where they should be, and the two GEMMs must agree. |
| Runtime API | device properties, driver/runtime version compatibility, `cudaMemGetInfo`, memcpy round trips up to 16 MiB, memset and device-to-device copy, pinned memory with async copies, events, `cudaStreamWaitEvent` ordering across streams, unified memory, 8 host threads sharing one device |
| Driver API | `cuInit`, primary context, allocation and copies, error names, a hand-written **PTX kernel** loaded with `cuModuleLoadData` and launched with `cuLaunchKernel`, driver/runtime agreement on device identity, and runtime allocations used from driver calls (shared primary context) |
| Negative paths | impossible allocation then recovery, double free, freeing a host pointer, null copy destination, invalid device ordinal, invalid launch configuration (block size 0 or 2048) followed by a good launch, zero-byte driver allocation, bogus driver free, missing kernel name, garbage module image |

Design points:
- **The oracle is tested separately.** CPU references and the `allclose` comparison live in a host-only library with 17 tests
  that run on any machine, so when a GPU test fails, the expectation is already known to be sound.
- **Launchers validate before launching** and return `cudaGetLastError()`, so a bad configuration is reported to the caller and
  cannot leave a sticky error behind. That validation is unit tested without a GPU.
- **Skip is explicit, and CI can forbid it.** With no GPU the suite skips with the reason (`no CUDA-capable device is detected`).
  `CUDATEST_REQUIRE_GPU=1` turns that into a failure so a broken GPU runner cannot pass by skipping everything.
- **Fat binaries.** Kernels are built for sm_75, 80, 86, 89 and 90 with PTX embedded, so newer GPUs JIT-compile them.
  Override with `-DCMAKE_CUDA_ARCHITECTURES=native` to build for just the local GPU.
- The PTX file (`cudatest/ptx/add_one.ptx`) is also assembled by `ptxas` as its own `ctest` entry, so a syntax error is caught
  without a GPU.

## Engineering notes

- **A bug the tests caught in the driver design.** The first `write()` copied user data straight into the shared
  buffer. `copy_from_user()` zero-fills the uncopied tail when it faults, so a single bad pointer would wipe existing
  data. The write path now copies into a bounce buffer first (`memdup_user`) and only then takes the lock and
  copies in. `bad_user_pointers_are_efault` asserts the data is still intact afterwards.
- **Locks are not held across user copies.** Reads snapshot under the mutex and call `copy_to_user()` after
  dropping it, so a page fault cannot stall other users of the device.
- **A scheduler bug that only real concurrency exposed.** With fake executors everything passed, but the gRPC
  integration test showed an agent being recorded as retired twice: both of its slots failed at the same moment.
  The fix is in `scheduler.cpp`, and a deterministic regression test forces two slots to fail together
  (`SeveralSlotsFailingAtOnceRetireTheAgentOnlyOnce`). I confirmed it fails when the fix is reverted.
- **Process cleanup is safe against pid reuse.** The runner detects exit with `waitid(WNOWAIT)`, kills the
  process group *while the zombie leader still reserves the group id*, and only then reaps. A background child that
  outlives its parent and holds the output pipe cannot hang a task or leak past it.
- **Warnings paid off twice.** `-Wc++20-compat` flagged a field named `requires` (a C++20 keyword), which would break a
  future standard bump; it is now `needs` in code while the JSON key stays `requires`. `-Werror` plus fortify caught a
  test that read 100 bytes into a 16-byte buffer.
- **A test that would have lied.** The first draft of the driver-version test asserted `driver >= runtime`. CUDA's
  minor-version compatibility lets a newer runtime run on an older driver of the same major release, so that assertion would
  fail on healthy machines. It now compares major versions only.
- **Layers are separable.** The scheduler talks to an `Executor` interface, so its retry, routing and failure logic
  is unit tested with fakes; the gRPC executor is tested separately against in-process agents on loopback.
- **CI** runs the unit and integration tests, an ASan/UBSan build, a ThreadSanitizer build of the concurrent core, and a
  compile check of the kernel module (`.github/workflows/ci.yml`).

## Status

| Piece | Verified how |
|---|---|
| `platcheck` | 24 unit tests against a fake sysfs tree; clean under ASan/UBSan; run on a real host |
| `vmtest` logic | 23 unit tests (parsing, JUnit, domain XML, runner classification); clean under ASan/UBSan; failure paths run against a host with no libvirt daemon |
| `vmtest` against a real guest | **Not run in the environment this was written in** (no KVM available). Run it on a KVM host and expect to tweak timeouts or image details. |
| `valdev.ko` | Compiles warning-free with `W=1` against Linux 6.8 headers. **Not loaded** in the environment this was written in. |
| `test_valdev` | Built with `-Wall -Wextra -Werror`; 15/15 against a userspace model of the device, and it fails when that model returns the wrong errno. Not yet run against the real module. |
| `dtest` core | 45 unit tests (process runner, allow-list policy, job parsing, scheduler with fakes, reports); clean under ASan/UBSan and ThreadSanitizer |
| `dtest` over gRPC | 10 integration tests with in-process agents on loopback (auth, timeouts, policy, concurrency limit, label routing, agent dying mid-run) plus a 6-scenario end-to-end script driving the real `dtest-agent` and `dtest-ctl` binaries; clean under ASan/UBSan |
| `dtest` at scale | **Not tested.** Everything above ran on one machine with one CPU. No multi-host runs, TLS, or long soaks yet. |
| `cudatest` host side | 17 unit tests for the CPU references and `allclose`; 2 GPU-suite tests that validate launcher arguments without a device; `ptxas` assembles the PTX file |
| `cudatest` kernels and API tests | Compile cleanly with nvcc 12.0 for sm_75/80/86/89/90 (SASS + PTX) and the test binary builds warning-free. The skip path and `CUDATEST_REQUIRE_GPU=1` were exercised. **The 89 GPU tests have never run on a GPU**: there was none in the environment this was written in. Expect to fix a few assertions about exact CUDA behaviour (error codes on impossible allocations, tolerances) on first contact with real hardware. |

## Layout

```
platcheck/   host readiness library, CLI and unit tests
vmtest/      libvirt harness: framework, guest-agent client, JUnit writer, tests, unit tests
kmod/        valdev.c, shared ioctl ABI header, TAP test program, run_in_vm.sh
dtest/       proto, process runner, policy, job parser, scheduler, gRPC agent + controller, tests, examples
cudatest/    CPU reference library, CUDA kernels + launchers, PTX, Runtime/Driver API and kernel tests
docs/        TEST_PLAN.md
scripts/     prepare_image.sh
```
