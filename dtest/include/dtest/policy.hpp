#pragma once

#include <string>
#include <vector>

namespace dtest {

// What an agent is willing to execute. An agent runs arbitrary commands on behalf
// of a remote caller, so by default it only runs programs on an explicit allow list.
struct Policy {
    // Each entry is an absolute path to one program, or a directory (trailing '/')
    // that contains allowed programs.
    std::vector<std::string> allow;
    bool allow_any = false;  // disables the check entirely (dangerous; opt-in only)
};

// Returns an empty string if `argv0` may run, otherwise the reason it was refused.
// Paths are canonicalised first (".." and symlinks resolved), so neither
// "/allowed/../../bin/sh" nor a symlink inside an allowed directory can escape it.
std::string check_command(const Policy& policy, const std::string& argv0);

}  // namespace dtest
