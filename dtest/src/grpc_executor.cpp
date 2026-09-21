#include "dtest/grpc_executor.hpp"

#include <grpcpp/grpcpp.h>

namespace dtest {

GrpcExecutor::GrpcExecutor(std::string token, std::chrono::seconds rpc_margin)
    : token_(std::move(token)), margin_(rpc_margin) {}

bool GrpcExecutor::connect(const std::string& address, std::chrono::seconds timeout, AgentInfo* out, std::string* error) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    auto stub = v1::Agent::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout);
    ctx.set_wait_for_ready(true);  // give a starting agent a moment instead of failing instantly
    if (!token_.empty()) ctx.AddMetadata("authorization", "Bearer " + token_);

    v1::InfoReply info;
    const grpc::Status st = stub->GetInfo(&ctx, v1::InfoRequest(), &info);
    if (!st.ok()) {
        if (error) *error = std::string(st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? "unreachable" : "rejected") +
                            ": " + st.error_message();
        return false;
    }

    out->address = address;
    out->hostname = info.hostname();
    out->slots = info.slots() ? info.slots() : 1;
    out->labels.assign(info.labels().begin(), info.labels().end());
    {
        std::lock_guard<std::mutex> lk(mu_);
        stubs_[address] = std::move(stub);
    }
    return true;
}

AttemptResult GrpcExecutor::run(const AgentInfo& agent, const Task& task) {
    AttemptResult res;
    v1::Agent::Stub* stub = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = stubs_.find(agent.address);
        if (it != stubs_.end()) stub = it->second.get();
    }
    if (!stub) {
        res.status = AttemptStatus::AgentError;
        res.message = "no connection to " + agent.address;
        return res;
    }

    v1::RunTaskRequest req;
    req.set_task_id(task.name);
    for (const auto& a : task.argv) req.add_argv(a);
    for (const auto& [k, v] : task.env) (*req.mutable_env())[k] = v;
    req.set_timeout_ms(static_cast<uint32_t>(task.timeout_s) * 1000u);

    grpc::ClientContext ctx;
    // The agent enforces the task timeout itself; the RPC deadline only fires if the agent hangs.
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(task.timeout_s) + margin_);
    if (!token_.empty()) ctx.AddMetadata("authorization", "Bearer " + token_);

    const auto start = std::chrono::steady_clock::now();
    v1::RunTaskReply rep;
    const grpc::Status st = stub->RunTask(&ctx, req, &rep);
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!st.ok()) {
        switch (st.error_code()) {
            // Infrastructure problems: the agent (or the path to it) is unusable.
            case grpc::StatusCode::UNAVAILABLE:
            case grpc::StatusCode::DEADLINE_EXCEEDED:
            case grpc::StatusCode::UNAUTHENTICATED:
            case grpc::StatusCode::CANCELLED:
            case grpc::StatusCode::INTERNAL:
            case grpc::StatusCode::UNIMPLEMENTED:
            case grpc::StatusCode::UNKNOWN:
                res.status = AttemptStatus::AgentError;
                break;
            // The agent is fine but declined this task (policy, bad request, busy).
            default:
                res.status = AttemptStatus::Failed;
        }
        res.message = st.error_message();
        return res;
    }

    res.exit_code = rep.exit_code();
    res.out = rep.stdout_data();
    res.err = rep.stderr_data();
    res.message = rep.message();
    if (rep.duration_ms() > 0) res.seconds = rep.duration_ms() / 1000.0;
    if (rep.output_truncated()) res.message += (res.message.empty() ? "" : "; ") + std::string("output truncated");

    switch (rep.outcome()) {
        case v1::RunTaskReply::EXITED:
            res.status = rep.exit_code() == 0 ? AttemptStatus::Passed : AttemptStatus::Failed;
            break;
        case v1::RunTaskReply::SIGNALED:
            res.status = AttemptStatus::Failed;
            res.message = "killed by signal " + std::to_string(rep.term_signal()) + (res.message.empty() ? "" : "; " + res.message);
            break;
        case v1::RunTaskReply::TIMED_OUT:
            res.status = AttemptStatus::TimedOut;
            break;
        default:  // LAUNCH_FAILED and anything unknown
            res.status = AttemptStatus::Failed;
            if (res.message.empty()) res.message = "agent could not launch the command";
    }
    return res;
}

}  // namespace dtest
