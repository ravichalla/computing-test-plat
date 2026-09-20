#include "vmtest/tests.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <ostream>

#include "vmtest/guest_exec.hpp"
#include "vmtest/util.hpp"

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace vmtest {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool agent_alive(const Domain& vm, int timeout_s) {
    try {
        vm.agent_command(R"({"execute":"guest-ping"})", timeout_s);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static bool state_becomes(const Domain& vm, DomState want, int timeout_s) {
    return wait_until(seconds(timeout_s), milliseconds(200), [&] { return vm.state() == want; });
}

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

Context::~Context() {
    try {
        release_vm();
    } catch (...) {
    }
}

Connection& Context::connection() {
    if (!conn_) conn_ = std::make_unique<Connection>(cfg.uri);
    return *conn_;
}

void Context::remove_file(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    fs::remove(path, ec);
}

std::string Context::make_overlay(unsigned id) {
    // The guest never writes to the base image: every VM gets a throwaway qcow2 overlay.
    const fs::path base = fs::absolute(cfg.image);
    const fs::path overlay = fs::path(cfg.work_dir) / ("vmtest-" + std::to_string(::getpid()) + "-" + std::to_string(id) + ".qcow2");
    const int rc = run_cmd({"qemu-img", "create", "-q", "-f", "qcow2", "-F", cfg.image_format, "-b", base.string(),
                            overlay.string()});
    if (rc == 127) throw TestFailure("qemu-img not found in PATH (install qemu-utils)");
    if (rc != 0) throw TestFailure("qemu-img create failed (rc=" + std::to_string(rc) + "); check --image and --work-dir");
    return overlay.string();
}

void Context::wait_for_agent(const Domain& vm) {
    const bool up = wait_until(seconds(cfg.boot_timeout_s), milliseconds(2000), [&] { return agent_alive(vm); });
    if (!up) {
        throw TestFailure("guest agent did not answer within " + std::to_string(cfg.boot_timeout_s) +
                          "s (is qemu-guest-agent installed and enabled in the image?)");
    }
}

Domain Context::boot_fresh_vm(std::string* overlay_out) {
    const unsigned id = ++next_id_;
    const std::string overlay = make_overlay(id);
    if (overlay_out) *overlay_out = overlay;
    try {
        const std::string name = "vmtest-" + std::to_string(::getpid()) + "-" + std::to_string(id);
        const std::string xml = build_domain_xml(name, cfg.vcpus, cfg.mem_mib, overlay, "qcow2");
        Domain vm = Domain::create_transient(connection(), xml, name);
        wait_for_agent(vm);
        return vm;
    } catch (...) {
        remove_file(overlay);
        throw;
    }
}

Domain& Context::ensure_vm() {
    if (!vm_ || !vm_->valid() || vm_->state() == DomState::Gone) {
        release_vm();
        vm_ = std::make_unique<Domain>(boot_fresh_vm(&vm_overlay_));
    }
    return *vm_;
}

void Context::release_vm() {
    if (vm_) {
        try {
            const auto st = vm_->state();
            if (st != DomState::Gone && st != DomState::ShutOff) vm_->destroy();
        } catch (...) {
        }
        vm_.reset();
    }
    remove_file(vm_overlay_);
    vm_overlay_.clear();
}

// ---------------------------------------------------------------------------
// test cases
// ---------------------------------------------------------------------------

static void t_boot_and_agent(Context& c) {
    Domain& vm = c.ensure_vm();
    require(vm.state() == DomState::Running, "domain is not running after boot: " + to_string(vm.state()));
    require(agent_alive(vm), "guest agent stopped answering");
    require_eq(vm.info().vcpus, c.cfg.vcpus, "vCPU count reported by libvirt");
}

static void t_pause_resume(Context& c) {
    Domain& vm = c.ensure_vm();
    vm.suspend();
    require(state_becomes(vm, DomState::Paused, 10), "domain did not reach 'paused'");
    vm.resume();
    require(state_becomes(vm, DomState::Running, 10), "domain did not return to 'running'");
    require(wait_until(seconds(30), seconds(1), [&] { return agent_alive(vm); }),
            "guest agent unreachable 30s after resume");
}

static void t_vcpu_count(Context& c) {
    Domain& vm = c.ensure_vm();
    const ExecResult r = guest_sh(vm, "nproc");
    require_eq(r.exit_code, 0, "nproc exit code");
    require_eq(std::stoi(r.out), static_cast<int>(c.cfg.vcpus), "vCPUs visible inside the guest");
}

static void t_memory(Context& c) {
    Domain& vm = c.ensure_vm();
    const ExecResult r = guest_sh(vm, "awk '/^MemTotal:/ {print $2}' /proc/meminfo");
    require_eq(r.exit_code, 0, "meminfo read exit code");
    const long guest_kib = std::stol(r.out);
    const long configured_kib = static_cast<long>(c.cfg.mem_mib) * 1024;
    // The kernel reserves some RAM, so MemTotal is a bit below the configured size.
    require(guest_kib <= configured_kib, "guest reports more memory than configured (" + std::to_string(guest_kib) + " kiB)");
    require(guest_kib * 100 >= configured_kib * 70,
            "guest reports less than 70% of configured memory (" + std::to_string(guest_kib) + " kiB)");
}

static void t_hypervisor_flag(Context& c) {
    Domain& vm = c.ensure_vm();
    const ExecResult r = guest_sh(vm, "grep -m1 '^flags' /proc/cpuinfo | grep -qw hypervisor && echo yes || echo no");
    require_eq(r.exit_code, 0, "cpuinfo probe exit code");
    require(r.out.find("yes") == 0, "guest CPU does not advertise the 'hypervisor' flag");
}

static void t_exit_code(Context& c) {
    Domain& vm = c.ensure_vm();
    const ExecResult r = guest_sh(vm, "exit 7");
    require(r.exited, "command did not finish");
    require_eq(r.exit_code, 7, "exit code propagated through the guest agent");
}

static void t_stdout_capture(Context& c) {
    Domain& vm = c.ensure_vm();
    const ExecResult r = guest_sh(vm, "printf hello-vmtest; printf oops >&2");
    require_eq(r.exit_code, 0, "exit code");
    require(r.out == "hello-vmtest", "stdout mismatch: '" + r.out + "'");
    require(r.err == "oops", "stderr mismatch: '" + r.err + "'");
}

static void t_graceful_shutdown(Context& c) {
    Domain& vm = c.ensure_vm();
    vm.shutdown();
    const bool stopped = wait_until(seconds(c.cfg.shutdown_timeout_s), milliseconds(1000), [&] {
        const auto st = vm.state();
        return st == DomState::Gone || st == DomState::ShutOff;
    });
    require(stopped, "guest did not shut down within " + std::to_string(c.cfg.shutdown_timeout_s) + "s");
    c.release_vm();
}

static void t_boot_destroy_soak(Context& c) {
    if (c.cfg.iterations <= 0) throw SkipTest("pass --iterations N to enable the soak test");
    c.release_vm();  // start from a clean slate
    for (int i = 1; i <= c.cfg.iterations; ++i) {
        std::string overlay;
        try {
            Domain vm = c.boot_fresh_vm(&overlay);
            require(agent_alive(vm), "iteration " + std::to_string(i) + ": guest agent not alive");
            vm.destroy();
            require(state_becomes(vm, DomState::Gone, 15) || vm.state() == DomState::ShutOff,
                    "iteration " + std::to_string(i) + ": domain still present after destroy");
        } catch (const std::exception& e) {
            Context::remove_file(overlay);
            throw TestFailure("iteration " + std::to_string(i) + "/" + std::to_string(c.cfg.iterations) + ": " + e.what());
        }
        Context::remove_file(overlay);
    }
}

std::vector<TestCase> build_tests() {
    return {
        {"lifecycle", "boot_and_agent_ping", t_boot_and_agent},
        {"lifecycle", "pause_resume", t_pause_resume},
        {"guest", "vcpu_count_matches", t_vcpu_count},
        {"guest", "memory_matches", t_memory},
        {"guest", "hypervisor_cpu_flag", t_hypervisor_flag},
        {"guest", "exit_code_propagates", t_exit_code},
        {"guest", "stdout_stderr_capture", t_stdout_capture},
        {"lifecycle", "graceful_shutdown", t_graceful_shutdown},
        {"stress", "boot_destroy_soak", t_boot_destroy_soak},
    };
}

bool matches_filter(const TestCase& t, const std::string& filter) {
    if (filter.empty()) return true;
    return (t.suite + "." + t.name).find(filter) != std::string::npos;
}

std::vector<TestResult> run_tests(Context& ctx, const std::vector<TestCase>& tests, std::ostream& log) {
    std::vector<TestResult> results;
    for (const auto& t : tests) {
        if (!matches_filter(t, ctx.cfg.filter)) continue;

        TestResult r;
        r.suite = t.suite;
        r.name = t.name;
        const auto start = std::chrono::steady_clock::now();
        try {
            t.fn(ctx);
            r.outcome = Outcome::Passed;
        } catch (const SkipTest& e) {
            r.outcome = Outcome::Skipped;
            r.message = e.what();
        } catch (const std::exception& e) {
            r.outcome = Outcome::Failed;
            r.message = e.what();
        }
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        const char* tag = r.outcome == Outcome::Passed ? "PASS" : r.outcome == Outcome::Failed ? "FAIL" : "SKIP";
        log << "[" << tag << "] " << t.suite << "." << t.name << " (" << std::fixed << std::setprecision(1)
            << r.seconds << "s)";
        if (!r.message.empty()) log << " - " << r.message;
        log << "\n" << std::flush;
        results.push_back(std::move(r));
    }
    ctx.release_vm();
    return results;
}

}  // namespace vmtest
