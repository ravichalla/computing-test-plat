#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "dtest.grpc.pb.h"
#include "dtest/scheduler.hpp"

namespace dtest {

// Executor that runs tasks on remote agents over gRPC.
//
// Transport is plaintext gRPC with a bearer token. Run it on a trusted network or
// through a tunnel (VPN, SSH port forward); it is not a substitute for TLS.
class GrpcExecutor final : public Executor {
public:
    explicit GrpcExecutor(std::string token, std::chrono::seconds rpc_margin = std::chrono::seconds(15));

    // Opens a channel to `address` and asks the agent to describe itself.
    // Returns false and sets *error if the agent is unreachable or refuses the token.
    bool connect(const std::string& address, std::chrono::seconds timeout, AgentInfo* out, std::string* error);

    AttemptResult run(const AgentInfo& agent, const Task& task) override;

private:
    std::string token_;
    std::chrono::seconds margin_;
    std::mutex mu_;
    std::map<std::string, std::unique_ptr<v1::Agent::Stub>> stubs_;
};

}  // namespace dtest
