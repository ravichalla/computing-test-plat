#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vmtest/virt.hpp"

namespace vmtest {

struct ExecResult {
    bool exited = false;
    int exit_code = -1;  // 128+N if the process was killed by signal N
    std::string out;
    std::string err;
};

// Parsers for the guest agent's JSON replies (pure, unit tested).
// Both throw std::runtime_error if the agent reported an error object.
std::optional<long> parse_exec_pid(const std::string& json);
ExecResult parse_exec_status(const std::string& json);

// Runs a program inside the guest via guest-exec and waits for it to finish.
ExecResult guest_exec(const Domain& vm, const std::string& path, const std::vector<std::string>& args,
                      int timeout_s = 30);

// Convenience: /bin/sh -c <script>
ExecResult guest_sh(const Domain& vm, const std::string& script, int timeout_s = 30);

}  // namespace vmtest
