#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmtest/config.hpp"
#include "vmtest/junit.hpp"
#include "vmtest/virt.hpp"

namespace vmtest {

// Thrown by a test to mark itself skipped / failed.
struct SkipTest : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void require(bool cond, const std::string& msg) {
    if (!cond) throw TestFailure(msg);
}

template <class A, class B>
void require_eq(const A& actual, const B& expected, const std::string& what) {
    if (!(actual == expected)) {
        throw TestFailure(what + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

// Shared state for a run: one libvirt connection and (lazily) one booted guest
// that the guest.* tests share so we only pay the boot cost once.
class Context {
public:
    explicit Context(const Config& c) : cfg(c) {}
    ~Context();

    const Config cfg;

    Domain& ensure_vm();                 // boots the shared guest on first use
    Domain boot_fresh_vm(std::string* overlay_out);  // boots an independent guest
    void release_vm();                   // destroys the shared guest and its overlay
    static void remove_file(const std::string& path);

private:
    Connection& connection();
    std::string make_overlay(unsigned id);
    void wait_for_agent(const Domain& vm);

    std::unique_ptr<Connection> conn_;
    std::unique_ptr<Domain> vm_;
    std::string vm_overlay_;
    unsigned next_id_ = 0;
};

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void(Context&)> fn;
};

std::vector<TestCase> build_tests();
bool matches_filter(const TestCase& t, const std::string& filter);
std::vector<TestResult> run_tests(Context& ctx, const std::vector<TestCase>& tests, std::ostream& log);

// True if the guest agent answers guest-ping.
bool agent_alive(const Domain& vm, int timeout_s = 2);

}  // namespace vmtest
