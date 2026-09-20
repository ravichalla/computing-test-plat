#include "vmtest/util.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <thread>

namespace vmtest {

std::string base64_decode(const std::string& in) {
    static const std::string kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const auto pos = kChars.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;  // skip whitespace / garbage
        val = ((val << 6) + static_cast<int>(pos)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

int run_cmd(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool wait_until(std::chrono::milliseconds timeout, std::chrono::milliseconds interval,
                const std::function<bool()>& pred) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(interval);
    }
}

}  // namespace vmtest
