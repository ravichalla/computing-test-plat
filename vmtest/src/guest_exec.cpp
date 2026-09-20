#include "vmtest/guest_exec.hpp"

#include <chrono>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "vmtest/util.hpp"

namespace vmtest {

using nlohmann::json;

namespace {

json parse_reply(const std::string& text) {
    json j = json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("guest agent returned invalid JSON: " + text);
    if (j.contains("error")) {
        const auto& e = j["error"];
        throw std::runtime_error("guest agent error: " + (e.contains("desc") ? e["desc"].get<std::string>() : e.dump()));
    }
    return j;
}

std::string decode_field(const json& r, const char* key) {
    if (!r.contains(key) || !r[key].is_string()) return {};
    return base64_decode(r[key].get<std::string>());
}

}  // namespace

std::optional<long> parse_exec_pid(const std::string& text) {
    const json j = parse_reply(text);
    if (!j.contains("return") || !j["return"].is_object() || !j["return"].contains("pid")) return std::nullopt;
    return j["return"]["pid"].get<long>();
}

ExecResult parse_exec_status(const std::string& text) {
    const json j = parse_reply(text);
    ExecResult res;
    if (!j.contains("return") || !j["return"].is_object()) return res;
    const json& r = j["return"];
    res.exited = r.value("exited", false);
    if (!res.exited) return res;
    if (r.contains("exitcode")) {
        res.exit_code = r["exitcode"].get<int>();
    } else if (r.contains("signal")) {
        res.exit_code = 128 + r["signal"].get<int>();
    }
    res.out = decode_field(r, "out-data");
    res.err = decode_field(r, "err-data");
    return res;
}

ExecResult guest_exec(const Domain& vm, const std::string& path, const std::vector<std::string>& args,
                      int timeout_s) {
    json cmd = {{"execute", "guest-exec"},
                {"arguments", {{"path", path}, {"arg", args}, {"capture-output", true}}}};
    const auto pid = parse_exec_pid(vm.agent_command(cmd.dump(), 10));
    if (!pid) throw std::runtime_error("guest-exec returned no pid");

    const json status_cmd = {{"execute", "guest-exec-status"}, {"arguments", {{"pid", *pid}}}};
    const std::string status_json = status_cmd.dump();

    ExecResult last;
    const bool done = wait_until(std::chrono::seconds(timeout_s), std::chrono::milliseconds(200), [&] {
        last = parse_exec_status(vm.agent_command(status_json, 10));
        return last.exited;
    });
    if (!done) throw std::runtime_error("guest command timed out after " + std::to_string(timeout_s) + "s: " + path);
    return last;
}

ExecResult guest_sh(const Domain& vm, const std::string& script, int timeout_s) {
    return guest_exec(vm, "/bin/sh", {"-c", script}, timeout_s);
}

}  // namespace vmtest
