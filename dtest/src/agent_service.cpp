#include "dtest/agent_service.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "dtest/process.hpp"

namespace dtest {

std::vector<std::string> detect_labels() {
    std::vector<std::string> labels = {"os=linux"};
    utsname u;
    if (uname(&u) == 0) labels.push_back(std::string("arch=") + u.machine);
    if (access("/dev/kvm", R_OK | W_OK) == 0) labels.push_back("kvm=true");
    return labels;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

AgentService::AgentService(Options opts) : opts_(std::move(opts)) {
    if (opts_.slots == 0) opts_.slots = 1;
    if (opts_.hostname.empty()) {
        char buf[256] = {0};
        if (gethostname(buf, sizeof buf - 1) == 0) opts_.hostname = buf;
    }
}

grpc::Status AgentService::authenticate(const grpc::ServerContext& ctx) const {
    if (opts_.token.empty()) return grpc::Status::OK;
    const auto& md = ctx.client_metadata();
    const auto it = md.find("authorization");
    if (it == md.end()) return {grpc::StatusCode::UNAUTHENTICATED, "missing authorization metadata"};
    const std::string got(it->second.data(), it->second.size());
    if (!constant_time_equals(got, "Bearer " + opts_.token)) {
        return {grpc::StatusCode::UNAUTHENTICATED, "invalid token"};
    }
    return grpc::Status::OK;
}

grpc::Status AgentService::GetInfo(grpc::ServerContext* ctx, const v1::InfoRequest*, v1::InfoReply* reply) {
    if (auto s = authenticate(*ctx); !s.ok()) return s;
    reply->set_hostname(opts_.hostname);
    reply->set_cpus(std::thread::hardware_concurrency());
    reply->set_slots(opts_.slots);
    for (const auto& l : opts_.labels) reply->add_labels(l);
    reply->set_version("0.1.0");
    return grpc::Status::OK;
}

grpc::Status AgentService::RunTask(grpc::ServerContext* ctx, const v1::RunTaskRequest* req, v1::RunTaskReply* reply) {
    if (auto s = authenticate(*ctx); !s.ok()) return s;

    if (req->argv_size() == 0 || req->argv(0).empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "argv must not be empty"};
    }
    if (req->timeout_ms() == 0 || req->timeout_ms() > 24u * 3600u * 1000u) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "timeout_ms must be in 1..86400000"};
    }
    if (const std::string why = check_command(opts_.policy, req->argv(0)); !why.empty()) {
        return {grpc::StatusCode::PERMISSION_DENIED, why};
    }

    // Enforce the slot limit so a misbehaving controller cannot overload the host.
    if (running_.fetch_add(1) >= opts_.slots) {
        running_.fetch_sub(1);
        return {grpc::StatusCode::RESOURCE_EXHAUSTED, "agent is at its concurrency limit"};
    }

    RunSpec spec;
    spec.argv.assign(req->argv().begin(), req->argv().end());
    for (const auto& [k, v] : req->env()) spec.env[k] = v;
    spec.timeout = std::chrono::milliseconds(req->timeout_ms());
    spec.max_output_bytes = opts_.max_output_bytes;

    const RunOutput out = run_process(spec);
    running_.fetch_sub(1);

    switch (out.outcome) {
        case RunOutcome::Exited: reply->set_outcome(v1::RunTaskReply::EXITED); break;
        case RunOutcome::Signaled: reply->set_outcome(v1::RunTaskReply::SIGNALED); break;
        case RunOutcome::TimedOut: reply->set_outcome(v1::RunTaskReply::TIMED_OUT); break;
        case RunOutcome::LaunchFailed: reply->set_outcome(v1::RunTaskReply::LAUNCH_FAILED); break;
    }
    reply->set_exit_code(out.exit_code);
    reply->set_term_signal(out.term_signal);
    reply->set_stdout_data(out.out);
    reply->set_stderr_data(out.err);
    reply->set_output_truncated(out.truncated);
    reply->set_duration_ms(static_cast<uint32_t>(out.duration.count()));
    reply->set_message(out.message);
    return grpc::Status::OK;
}

}  // namespace dtest
