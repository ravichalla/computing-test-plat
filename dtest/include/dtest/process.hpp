#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace dtest {

struct RunSpec {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;  // merged over a minimal clean environment
    std::chrono::milliseconds timeout{60000};
    size_t max_output_bytes = 1 << 20;       // per stream; excess is discarded
};

enum class RunOutcome { Exited, Signaled, TimedOut, LaunchFailed };

struct RunOutput {
    RunOutcome outcome = RunOutcome::LaunchFailed;
    int exit_code = -1;    // valid when Exited
    int term_signal = 0;   // valid when Signaled
    std::string out;
    std::string err;
    bool truncated = false;
    std::chrono::milliseconds duration{0};
    std::string message;   // detail for LaunchFailed
};

// Runs a command in its own process group, captures stdout and stderr, and
// enforces the timeout by killing the whole group (SIGTERM, then SIGKILL after
// a short grace period). Never throws.
RunOutput run_process(const RunSpec& spec);

std::string to_string(RunOutcome o);

}  // namespace dtest
