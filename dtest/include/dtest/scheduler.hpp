#pragma once

#include <string>
#include <vector>

#include "dtest/jobs.hpp"

namespace dtest {

struct AgentInfo {
    std::string address;   // how the controller reaches it, e.g. "10.0.0.5:7001"
    std::string hostname;  // what the agent calls itself (for reports)
    unsigned slots = 1;    // concurrent tasks it accepts
    std::vector<std::string> labels;
};

// How a single attempt at running a task ended.
//   Failed / TimedOut  -> the TEST failed: counts against the task's retry budget.
//   AgentError         -> the INFRASTRUCTURE failed (unreachable, auth): the agent is
//                         retired and the task is rescheduled without spending a retry.
enum class AttemptStatus { Passed, Failed, TimedOut, AgentError };

struct AttemptResult {
    AttemptStatus status = AttemptStatus::Failed;
    int exit_code = -1;
    std::string out;
    std::string err;
    std::string message;
    double seconds = 0;
};

// Abstraction over "run this task on that agent"; the gRPC implementation lives in
// grpc_executor.hpp, tests use a fake. Must be thread-safe.
class Executor {
public:
    virtual ~Executor() = default;
    virtual AttemptResult run(const AgentInfo& agent, const Task& task) = 0;
};

struct AttemptRecord {
    std::string agent;  // agent address (unique, unlike hostnames)
    AttemptResult result;
};

enum class FinalStatus { Passed, Failed, TimedOut, Unschedulable };

struct TaskOutcome {
    Task task;
    FinalStatus status = FinalStatus::Unschedulable;
    std::vector<AttemptRecord> attempts;
    std::string message;  // set for Unschedulable

    // Passed, but only after at least one failed attempt.
    bool flaky() const;
};

struct RunSummary {
    std::vector<TaskOutcome> outcomes;      // same order as the input tasks
    std::vector<std::string> dead_agents;   // agents retired after an infrastructure error
    double wall_seconds = 0;

    int count(FinalStatus s) const;
    int flaky_count() const;
    bool all_passed() const;
};

std::string to_string(FinalStatus s);

// Runs tasks across agents. Each agent runs up to `slots` tasks at once.
//  - A task only runs on agents whose labels satisfy its `requires`.
//  - A failed test is retried (up to task.retries) preferably on a DIFFERENT agent.
//  - An agent that errors at the infrastructure level is retired, and its task is
//    rescheduled elsewhere without consuming a retry.
//  - A task nobody can run is reported Unschedulable rather than hanging the run.
class Scheduler {
public:
    Scheduler(std::vector<AgentInfo> agents, Executor& executor);
    RunSummary run(const std::vector<Task>& tasks);

private:
    std::vector<AgentInfo> agents_;
    Executor& exec_;
};

}  // namespace dtest
