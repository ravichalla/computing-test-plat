#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "dtest.grpc.pb.h"
#include "dtest/policy.hpp"

namespace dtest {

// Labels the agent can work out for itself: os, arch and whether /dev/kvm is usable.
std::vector<std::string> detect_labels();

// Constant-time string comparison (for token checks).
bool constant_time_equals(const std::string& a, const std::string& b);

class AgentService final : public v1::Agent::Service {
public:
    struct Options {
        std::string token;  // empty => authentication disabled (the caller must opt in explicitly)
        Policy policy;
        unsigned slots = 1;
        std::vector<std::string> labels;
        std::string hostname;
        size_t max_output_bytes = 1 << 20;
    };

    explicit AgentService(Options opts);

    grpc::Status GetInfo(grpc::ServerContext* ctx, const v1::InfoRequest* req, v1::InfoReply* reply) override;
    grpc::Status RunTask(grpc::ServerContext* ctx, const v1::RunTaskRequest* req, v1::RunTaskReply* reply) override;

private:
    grpc::Status authenticate(const grpc::ServerContext& ctx) const;

    Options opts_;
    std::atomic<unsigned> running_{0};
};

}  // namespace dtest
