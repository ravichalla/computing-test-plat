#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "dtest/process.hpp"

using namespace dtest;
using namespace std::chrono_literals;

namespace {

RunSpec sh(const std::string& script, std::chrono::milliseconds timeout = 5000ms) {
    RunSpec s;
    s.argv = {"/bin/sh", "-c", script};
    s.timeout = timeout;
    return s;
}

// A killed process can linger as a zombie until its parent reaps it, so "alive" means
// "exists and is not a zombie".
bool process_alive(long pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in) return false;
    std::string line;
    std::getline(in, line);
    const auto p = line.rfind(") ");
    return p != std::string::npos && p + 2 < line.size() && line[p + 2] != 'Z';
}

}  // namespace

TEST(Process, CapturesStdoutAndStderrSeparately) {
    const auto r = run_process(sh("echo out; echo err >&2"));
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.out, "out\n");
    EXPECT_EQ(r.err, "err\n");
    EXPECT_FALSE(r.truncated);
}

TEST(Process, ExitCodeIsReported) {
    const auto r = run_process(sh("exit 3"));
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.exit_code, 3);
}

TEST(Process, KilledBySignalIsReported) {
    const auto r = run_process(sh("kill -9 $$"));
    EXPECT_EQ(r.outcome, RunOutcome::Signaled);
    EXPECT_EQ(r.term_signal, 9);
}

TEST(Process, TimeoutKillsTheProcess) {
    const auto r = run_process(sh("sleep 30", 300ms));
    EXPECT_EQ(r.outcome, RunOutcome::TimedOut);
    EXPECT_LT(r.duration, 3000ms);
    EXPECT_NE(r.message.find("timeout"), std::string::npos);
}

TEST(Process, TimeoutKillsTheWholeProcessGroup) {
    char tmpl[] = "/tmp/dtest-pid-XXXXXX";
    const int fd = mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    close(fd);

    // The shell starts a background sleeper, records its pid, then waits.
    const auto r = run_process(sh(std::string("sleep 30 & echo $! > ") + tmpl + "; wait", 500ms));
    EXPECT_EQ(r.outcome, RunOutcome::TimedOut);

    std::ifstream in(tmpl);
    long pid = 0;
    in >> pid;
    std::remove(tmpl);
    ASSERT_GT(pid, 0);
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(process_alive(pid)) << "background child " << pid << " survived the timeout";
}

TEST(Process, BackgroundChildHoldingThePipeDoesNotBlockCompletion) {
    // The shell exits immediately but its child inherits stdout and would keep it open for 30 s.
    const auto r = run_process(sh("sleep 30 & echo done", 20000ms));
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.out, "done\n");
    EXPECT_LT(r.duration, 3000ms);
}

TEST(Process, MissingBinaryIsALaunchFailureNotExit127) {
    RunSpec s;
    s.argv = {"/nonexistent/binary"};
    const auto r = run_process(s);
    EXPECT_EQ(r.outcome, RunOutcome::LaunchFailed);
    EXPECT_NE(r.message.find("No such file"), std::string::npos) << r.message;
}

TEST(Process, EmptyArgvIsALaunchFailure) {
    const auto r = run_process(RunSpec{});
    EXPECT_EQ(r.outcome, RunOutcome::LaunchFailed);
}

TEST(Process, ScriptExitting127IsStillJustAnExit) {
    const auto r = run_process(sh("exit 127"));
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.exit_code, 127);
}

TEST(Process, OutputIsTruncatedAtTheLimit) {
    RunSpec s = sh("head -c 2000000 /dev/zero | tr '\\0' x");
    s.max_output_bytes = 1000;
    const auto r = run_process(s);
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.out.size(), 1000u);
    EXPECT_TRUE(r.truncated);
}

TEST(Process, EnvironmentIsCleanPlusRequested) {
    RunSpec s = sh("echo \"$FOO ${HOME:-unset}\"");
    s.env["FOO"] = "bar";
    const auto r = run_process(s);
    EXPECT_EQ(r.out, "bar unset\n");
}

TEST(Process, StdinIsDevNull) {
    const auto r = run_process(sh("cat; echo done", 3000ms));
    EXPECT_EQ(r.outcome, RunOutcome::Exited);
    EXPECT_EQ(r.out, "done\n");
}

TEST(Process, ConcurrentRunsDoNotInterfere) {
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([&, i] {
            const auto r = run_process(sh("sleep 0.3; echo " + std::to_string(i)));
            if (r.outcome == RunOutcome::Exited && r.out == std::to_string(i) + "\n") ++ok;
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), 8);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}
