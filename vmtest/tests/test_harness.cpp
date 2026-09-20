// Unit tests for the harness's own logic. None of these need a libvirt daemon,
// KVM, or a VM image, so they run anywhere (including CI).

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <sstream>

#include "vmtest/config.hpp"
#include "vmtest/guest_exec.hpp"
#include "vmtest/junit.hpp"
#include "vmtest/tests.hpp"
#include "vmtest/util.hpp"
#include "vmtest/virt.hpp"

using namespace vmtest;

// ---- util -------------------------------------------------------------------

TEST(Util, Base64Decode) {
    EXPECT_EQ(base64_decode("aGVsbG8="), "hello");
    EXPECT_EQ(base64_decode("aGVsbG8gd29ybGQ=\n"), "hello world");
    EXPECT_EQ(base64_decode(""), "");
    EXPECT_EQ(base64_decode("YQ=="), "a");
    EXPECT_EQ(base64_decode("YWI="), "ab");
    EXPECT_EQ(base64_decode("YWJj"), "abc");
}

TEST(Util, XmlEscape) {
    EXPECT_EQ(xml_escape("a&b<c>\"d'"), "a&amp;b&lt;c&gt;&quot;d&apos;");
    EXPECT_EQ(xml_escape("plain"), "plain");
}

TEST(Util, WaitUntilSucceedsAfterSeveralPolls) {
    std::atomic<int> calls{0};
    const bool ok = wait_until(std::chrono::milliseconds(2000), std::chrono::milliseconds(5),
                               [&] { return ++calls >= 3; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(calls.load(), 3);
}

TEST(Util, WaitUntilTimesOut) {
    const bool ok = wait_until(std::chrono::milliseconds(50), std::chrono::milliseconds(10), [] { return false; });
    EXPECT_FALSE(ok);
}

TEST(Util, RunCmdReportsExitStatus) {
    EXPECT_EQ(run_cmd({"true"}), 0);
    EXPECT_EQ(run_cmd({"false"}), 1);
    EXPECT_EQ(run_cmd({"/nonexistent/binary"}), 127);
    EXPECT_EQ(run_cmd({}), -1);
}

// ---- JUnit ------------------------------------------------------------------

TEST(Junit, CountsAndStructure) {
    std::vector<TestResult> rs = {
        {"lifecycle", "boot", Outcome::Passed, "", 1.5},
        {"lifecycle", "pause", Outcome::Failed, "state was <paused> & stuck", 0.25},
        {"stress", "soak", Outcome::Skipped, "not enabled", 0.0},
    };
    const std::string xml = render_junit("vmtest", rs);

    EXPECT_NE(xml.find("<testsuites name=\"vmtest\" tests=\"3\" failures=\"1\" skipped=\"1\""), std::string::npos);
    EXPECT_NE(xml.find("<testsuite name=\"lifecycle\" tests=\"2\" failures=\"1\""), std::string::npos);
    EXPECT_NE(xml.find("<testsuite name=\"stress\" tests=\"1\" failures=\"0\" skipped=\"1\""), std::string::npos);
    // messages must be XML-escaped
    EXPECT_NE(xml.find("message=\"state was &lt;paused&gt; &amp; stuck\""), std::string::npos);
    EXPECT_NE(xml.find("<skipped message=\"not enabled\"/>"), std::string::npos);
}

TEST(Junit, EmptyRunIsValid) {
    const std::string xml = render_junit("vmtest", {});
    EXPECT_NE(xml.find("tests=\"0\""), std::string::npos);
    EXPECT_NE(xml.find("</testsuites>"), std::string::npos);
}

// ---- domain XML -------------------------------------------------------------

TEST(DomainXml, ContainsExpectedDevices) {
    const std::string xml = build_domain_xml("vm-1", 4, 2048, "/var/tmp/o.qcow2", "qcow2");
    EXPECT_NE(xml.find("<domain type='kvm'>"), std::string::npos);
    EXPECT_NE(xml.find("<name>vm-1</name>"), std::string::npos);
    EXPECT_NE(xml.find("<vcpu>4</vcpu>"), std::string::npos);
    EXPECT_NE(xml.find("<memory unit='MiB'>2048</memory>"), std::string::npos);
    EXPECT_NE(xml.find("org.qemu.guest_agent.0"), std::string::npos);  // guest-agent channel
    EXPECT_NE(xml.find("source file='/var/tmp/o.qcow2'"), std::string::npos);
    EXPECT_NE(xml.find("bus='virtio'"), std::string::npos);
}

TEST(DomainXml, EscapesUntrustedValues) {
    const std::string xml = build_domain_xml("a<b", 1, 128, "/tmp/x'&y.qcow2", "qcow2");
    EXPECT_EQ(xml.find("a<b"), std::string::npos);
    EXPECT_NE(xml.find("a&lt;b"), std::string::npos);
    EXPECT_NE(xml.find("x&apos;&amp;y"), std::string::npos);
}

// ---- guest agent reply parsing ---------------------------------------------

TEST(GuestExec, ParsePid) {
    EXPECT_EQ(parse_exec_pid(R"({"return":{"pid":4242}})"), 4242);
    EXPECT_FALSE(parse_exec_pid(R"({"return":{}})").has_value());
}

TEST(GuestExec, ParseStatusNotYetExited) {
    const auto r = parse_exec_status(R"({"return":{"exited":false}})");
    EXPECT_FALSE(r.exited);
}

TEST(GuestExec, ParseStatusDecodesOutput) {
    // "hello" and "oops" base64-encoded
    const auto r = parse_exec_status(R"({"return":{"exited":true,"exitcode":3,"out-data":"aGVsbG8=","err-data":"b29wcw=="}})");
    EXPECT_TRUE(r.exited);
    EXPECT_EQ(r.exit_code, 3);
    EXPECT_EQ(r.out, "hello");
    EXPECT_EQ(r.err, "oops");
}

TEST(GuestExec, ParseStatusSignalMapsTo128PlusN) {
    const auto r = parse_exec_status(R"({"return":{"exited":true,"signal":9}})");
    EXPECT_TRUE(r.exited);
    EXPECT_EQ(r.exit_code, 137);
}

TEST(GuestExec, AgentErrorBecomesException) {
    EXPECT_THROW(parse_exec_status(R"({"error":{"class":"GenericError","desc":"no such pid"}})"), std::runtime_error);
    EXPECT_THROW(parse_exec_pid("not json"), std::runtime_error);
}

// ---- CLI parsing ------------------------------------------------------------

namespace {
ParseResult parse(std::vector<const char*> args) {
    args.insert(args.begin(), "vmtest");
    return parse_args(static_cast<int>(args.size()), args.data());
}
}  // namespace

TEST(Config, Defaults) {
    const auto r = parse({});
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(r.cfg.uri, "qemu:///system");
    EXPECT_EQ(r.cfg.vcpus, 2u);
    EXPECT_EQ(r.cfg.mem_mib, 1024u);
    EXPECT_EQ(r.cfg.iterations, 0);
}

TEST(Config, AllOptions) {
    const auto r = parse({"--image", "/i.qcow2", "--uri", "qemu:///session", "--vcpus", "4", "--memory-mib", "4096",
                          "--boot-timeout", "60", "--iterations", "5", "--filter", "guest", "--junit", "out.xml"});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(r.cfg.image, "/i.qcow2");
    EXPECT_EQ(r.cfg.uri, "qemu:///session");
    EXPECT_EQ(r.cfg.vcpus, 4u);
    EXPECT_EQ(r.cfg.mem_mib, 4096u);
    EXPECT_EQ(r.cfg.boot_timeout_s, 60);
    EXPECT_EQ(r.cfg.iterations, 5);
    EXPECT_EQ(r.cfg.filter, "guest");
    EXPECT_EQ(r.cfg.junit_path, "out.xml");
}

TEST(Config, RejectsBadInput) {
    EXPECT_FALSE(parse({"--vcpus"}).error.empty());             // missing value
    EXPECT_FALSE(parse({"--vcpus", "abc"}).error.empty());      // not a number
    EXPECT_FALSE(parse({"--vcpus", "0"}).error.empty());        // out of range
    EXPECT_FALSE(parse({"--memory-mib", "64"}).error.empty());  // too small
    EXPECT_FALSE(parse({"--bogus"}).error.empty());             // unknown option
}

TEST(Config, HelpAndList) {
    EXPECT_TRUE(parse({"--help"}).help);
    EXPECT_TRUE(parse({"--list"}).cfg.list_only);
}

// ---- test registry and runner ----------------------------------------------

TEST(Registry, TestNamesAreUniqueAndFilterWorks) {
    const auto tests = build_tests();
    ASSERT_FALSE(tests.empty());
    std::set<std::string> names;
    for (const auto& t : tests) EXPECT_TRUE(names.insert(t.suite + "." + t.name).second) << "duplicate " << t.name;

    EXPECT_TRUE(matches_filter(tests[0], ""));
    EXPECT_TRUE(matches_filter({"guest", "vcpu_count_matches", nullptr}, "vcpu"));
    EXPECT_FALSE(matches_filter({"guest", "vcpu_count_matches", nullptr}, "lifecycle"));
}

TEST(Runner, ClassifiesPassFailSkip) {
    Config cfg;
    Context ctx(cfg);
    const std::vector<TestCase> cases = {
        {"t", "ok", [](Context&) {}},
        {"t", "bad", [](Context&) { require(false, "boom"); }},
        {"t", "unexpected", [](Context&) { throw std::runtime_error("kaboom"); }},
        {"t", "skip", [](Context&) { throw SkipTest("nope"); }},
    };
    std::ostringstream log;
    const auto rs = run_tests(ctx, cases, log);

    ASSERT_EQ(rs.size(), 4u);
    EXPECT_EQ(rs[0].outcome, Outcome::Passed);
    EXPECT_EQ(rs[1].outcome, Outcome::Failed);
    EXPECT_EQ(rs[1].message, "boom");
    EXPECT_EQ(rs[2].outcome, Outcome::Failed);  // any exception is a failure, not a crash
    EXPECT_EQ(rs[3].outcome, Outcome::Skipped);
    EXPECT_NE(log.str().find("[FAIL] t.bad"), std::string::npos);
}

TEST(Runner, HonoursFilter) {
    Config cfg;
    cfg.filter = "keep";
    Context ctx(cfg);
    int ran = 0;
    const std::vector<TestCase> cases = {
        {"s", "keep_me", [&](Context&) { ++ran; }},
        {"s", "drop_me", [&](Context&) { ++ran; }},
    };
    std::ostringstream log;
    const auto rs = run_tests(ctx, cases, log);
    EXPECT_EQ(rs.size(), 1u);
    EXPECT_EQ(ran, 1);
}

TEST(Runner, SoakTestSkipsWithoutIterations) {
    Config cfg;  // iterations == 0
    Context ctx(cfg);
    std::vector<TestCase> soak;
    for (auto& t : build_tests()) {
        if (t.name == "boot_destroy_soak") soak.push_back(t);
    }
    ASSERT_EQ(soak.size(), 1u);
    std::ostringstream log;
    const auto rs = run_tests(ctx, soak, log);
    ASSERT_EQ(rs.size(), 1u);
    EXPECT_EQ(rs[0].outcome, Outcome::Skipped);
}

TEST(Assertions, RequireEqMessage) {
    try {
        require_eq(3, 4, "vCPUs");
        FAIL() << "expected TestFailure";
    } catch (const TestFailure& e) {
        EXPECT_STREQ(e.what(), "vCPUs: expected 4, got 3");
    }
}
