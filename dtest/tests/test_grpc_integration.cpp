// Integration tests over real gRPC: agents run in-process on ephemeral loopback ports,
// the controller side uses the real GrpcExecutor and Scheduler.

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "dtest/agent_service.hpp"
#include "dtest/grpc_executor.hpp"
#include "dtest/report.hpp"
#include "dtest/scheduler.hpp"

using namespace dtest;
using namespace std::chrono_literals;

namespace {

constexpr const char* kToken = "test-secret";

struct TestAgent {
    std::unique_ptr<AgentService> service;
    std::unique_ptr<grpc::Server> server;
    int port = 0;

    std::string addr() const { return "127.0.0.1:" + std::to_string(port); }
    void stop() {
        if (server) server->Shutdown(std::chrono::system_clock::now() + 1s);
        server.reset();
    }
    ~TestAgent() { stop(); }
};

AgentService::Options default_opts(std::vector<std::string> labels = {"role=a"}) {
    AgentService::Options o;
    o.token = kToken;
    o.policy.allow = {"/bin/", "/usr/bin/"};
    o.slots = 2;
    o.labels = std::move(labels);
    return o;
}

std::unique_ptr<TestAgent> start_agent(AgentService::Options opts) {
    auto a = std::make_unique<TestAgent>();
    a->service = std::make_unique<AgentService>(std::move(opts));
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &a->port);
    b.RegisterService(a->service.get());
    a->server = b.BuildAndStart();
    EXPECT_TRUE(a->server != nullptr);
    EXPECT_GT(a->port, 0);
    return a;
}

Task sh_task(const std::string& name, const std::string& script, int timeout_s = 10, int retries = 0) {
    Task t;
    t.name = name;
    t.argv = {"/bin/sh", "-c", script};
    t.timeout_s = timeout_s;
    t.retries = retries;
    return t;
}

}  // namespace

TEST(GrpcAgent, ConnectFetchesHostInfo) {
    auto a = start_agent(default_opts({"role=a", "kvm=true"}));
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;
    EXPECT_EQ(info.address, a->addr());
    EXPECT_FALSE(info.hostname.empty());
    EXPECT_EQ(info.slots, 2u);
    EXPECT_EQ(info.labels, (std::vector<std::string>{"role=a", "kvm=true"}));
}

TEST(GrpcAgent, WrongOrMissingTokenIsRejected) {
    auto a = start_agent(default_opts());
    AgentInfo info;
    std::string err;

    GrpcExecutor wrong("not-the-token");
    EXPECT_FALSE(wrong.connect(a->addr(), 3s, &info, &err));
    EXPECT_NE(err.find("rejected"), std::string::npos) << err;

    GrpcExecutor none("");
    EXPECT_FALSE(none.connect(a->addr(), 3s, &info, &err));
}

TEST(GrpcAgent, UnreachableAgentFailsFast) {
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(ex.connect("127.0.0.1:1", 1s, &info, &err));  // nothing listens on port 1
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
    EXPECT_NE(err.find("unreachable"), std::string::npos) << err;
}

TEST(GrpcAgent, RunsTasksAndReportsPassFailAndOutput) {
    auto a = start_agent(default_opts());
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;

    Scheduler s({info}, ex);
    const auto sum = s.run({sh_task("ok", "echo hi"), sh_task("bad", "echo out; echo err >&2; exit 3")});

    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Passed);
    EXPECT_EQ(sum.outcomes[0].attempts[0].result.out, "hi\n");

    const auto& bad = sum.outcomes[1];
    EXPECT_EQ(bad.status, FinalStatus::Failed);
    EXPECT_EQ(bad.attempts[0].result.exit_code, 3);
    EXPECT_EQ(bad.attempts[0].result.out, "out\n");
    EXPECT_EQ(bad.attempts[0].result.err, "err\n");
}

TEST(GrpcAgent, TimeoutIsEnforcedOnTheAgentAndReportedAsTimedOut) {
    auto a = start_agent(default_opts());
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;

    Scheduler s({info}, ex);
    const auto start = std::chrono::steady_clock::now();
    const auto sum = s.run({sh_task("hangs", "sleep 30", /*timeout_s=*/1)});
    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 8s);
    EXPECT_TRUE(sum.dead_agents.empty()) << "a slow TEST must not retire the agent";
}

TEST(GrpcAgent, PolicyViolationFailsTheTaskButNotTheAgent) {
    auto a = start_agent(default_opts());
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;

    Task forbidden;
    forbidden.name = "forbidden";
    forbidden.argv = {"/opt/not-allowed/tool"};
    Task relative;
    relative.name = "relative";
    relative.argv = {"sh", "-c", "true"};

    Scheduler s({info}, ex);
    const auto sum = s.run({forbidden, relative, sh_task("fine", "true")});

    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Failed);
    EXPECT_NE(sum.outcomes[0].attempts[0].result.message.find("not permitted"), std::string::npos);
    EXPECT_EQ(sum.outcomes[1].status, FinalStatus::Failed);
    EXPECT_NE(sum.outcomes[1].attempts[0].result.message.find("absolute"), std::string::npos);
    EXPECT_EQ(sum.outcomes[2].status, FinalStatus::Passed);  // the agent is still healthy
    EXPECT_TRUE(sum.dead_agents.empty());
}

TEST(GrpcAgent, MissingBinaryOnAnAllowedPathIsAFailedTask) {
    auto opts = default_opts();
    opts.policy.allow.push_back("/definitely/not/here/");
    auto a = start_agent(opts);
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;

    Task t;
    t.name = "missing";
    t.argv = {"/definitely/not/here/tool"};
    Scheduler s({info}, ex);
    const auto sum = s.run({t});
    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Failed);
    EXPECT_NE(sum.outcomes[0].attempts[0].result.message.find("No such file"), std::string::npos)
        << sum.outcomes[0].attempts[0].result.message;
}

TEST(GrpcAgent, AgentEnforcesItsOwnConcurrencyLimit) {
    auto opts = default_opts();
    opts.slots = 1;
    auto a = start_agent(opts);
    GrpcExecutor ex(kToken);
    AgentInfo info;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &info, &err)) << err;
    info.slots = 2;  // a controller that lies about capacity must be stopped by the agent

    const Task t = sh_task("sleepy", "sleep 1");
    AttemptResult r1, r2;
    std::thread t1([&] { r1 = ex.run(info, t); });
    std::thread t2([&] { r2 = ex.run(info, t); });
    t1.join();
    t2.join();

    const int passed = (r1.status == AttemptStatus::Passed) + (r2.status == AttemptStatus::Passed);
    EXPECT_EQ(passed, 1);
    const AttemptResult& rejected = r1.status == AttemptStatus::Passed ? r2 : r1;
    EXPECT_EQ(rejected.status, AttemptStatus::Failed);
    EXPECT_NE(rejected.message.find("concurrency limit"), std::string::npos) << rejected.message;
}

TEST(GrpcCluster, LabelsRouteTasksToTheRightAgent) {
    auto a = start_agent(default_opts({"role=a"}));
    auto b = start_agent(default_opts({"role=b"}));
    GrpcExecutor ex(kToken);
    AgentInfo ia, ib;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &ia, &err)) << err;
    ASSERT_TRUE(ex.connect(b->addr(), 3s, &ib, &err)) << err;

    std::vector<Task> ts;
    for (int i = 0; i < 6; ++i) {
        Task t = sh_task("b" + std::to_string(i), "true");
        t.needs = {"role=b"};
        ts.push_back(t);
    }
    Scheduler s({ia, ib}, ex);
    const auto sum = s.run(ts);
    EXPECT_TRUE(sum.all_passed());
    for (const auto& o : sum.outcomes) EXPECT_EQ(o.attempts.back().agent, b->addr());
}

TEST(GrpcCluster, AgentDisappearingMidRunIsAbsorbed) {
    auto a = start_agent(default_opts({"role=a"}));
    auto b = start_agent(default_opts({"role=b"}));
    GrpcExecutor ex(kToken);
    AgentInfo ia, ib;
    std::string err;
    ASSERT_TRUE(ex.connect(a->addr(), 3s, &ia, &err)) << err;
    ASSERT_TRUE(ex.connect(b->addr(), 3s, &ib, &err)) << err;

    const std::string dead_addr = a->addr();
    a->stop();  // agent A goes away after the controller has already connected to it

    std::vector<Task> ts;
    for (int i = 0; i < 8; ++i) ts.push_back(sh_task("t" + std::to_string(i), "true", 10, /*retries=*/0));
    Scheduler s({ia, ib}, ex);
    const auto sum = s.run(ts);

    EXPECT_TRUE(sum.all_passed()) << render_text(sum);
    ASSERT_EQ(sum.dead_agents.size(), 1u);
    EXPECT_EQ(sum.dead_agents[0], dead_addr);
    for (const auto& o : sum.outcomes) EXPECT_EQ(o.attempts.back().agent, b->addr());
}
