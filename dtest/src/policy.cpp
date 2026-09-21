#include "dtest/policy.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace dtest {

namespace {

std::string canonical(const std::string& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::path(p), ec);
    if (ec) c = fs::path(p).lexically_normal();
    return c.string();
}

}  // namespace

std::string check_command(const Policy& policy, const std::string& argv0) {
    if (policy.allow_any) return "";
    if (argv0.empty() || argv0[0] != '/') {
        return "command must be an absolute path (got '" + argv0 + "')";
    }
    const std::string target = canonical(argv0);

    for (const auto& entry : policy.allow) {
        if (entry.empty()) continue;
        const bool is_dir = entry.back() == '/';
        const std::string allowed = canonical(entry);
        if (is_dir) {
            const std::string prefix = allowed.back() == '/' ? allowed : allowed + "/";
            if (target.size() > prefix.size() && target.compare(0, prefix.size(), prefix) == 0) return "";
        } else if (target == allowed) {
            return "";
        }
    }
    return "command '" + argv0 + "' is not permitted by this agent's allow list";
}

}  // namespace dtest
