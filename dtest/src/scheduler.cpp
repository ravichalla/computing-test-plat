#include "dtest/scheduler.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

namespace dtest {

bool TaskOutcome::flaky() const {
    if (status != FinalStatus::Passed) return false;
    for (const auto& a : attempts) {
        if (a.result.status == AttemptStatus::Failed || a.result.status == AttemptStatus::TimedOut) return true;
    }
    return false;
}

int RunSummary::count(FinalStatus s) const {
    int n = 0;
    for (const auto& o : outcomes) n += (o.status == s);
    return n;
}

int RunSummary::flaky_count() const {
    int n = 0;
    for (const auto& o : outcomes) n += o.flaky();
    return n;
}

bool RunSummary::all_passed() const { return count(FinalStatus::Passed) == static_cast<int>(outcomes.size()); }

std::string to_string(FinalStatus s) {
    switch (s) {
        case FinalStatus::Passed: return "passed";
        case FinalStatus::Failed: return "failed";
        case FinalStatus::TimedOut: return "timed-out";
        case FinalStatus::Unschedulable: return "unschedulable";
    }
    return "?";
}

Scheduler::Scheduler(std::vector<AgentInfo> agents, Executor& executor)
    : agents_(std::move(agents)), exec_(executor) {
    for (auto& a : agents_) {
        if (a.slots == 0) a.slots = 1;
    }
}

namespace {

struct TaskState {
    int failures = 0;
    std::set<size_t> tried;  // agent indices already used
    bool running = false;
    bool done = false;
    FinalStatus final = FinalStatus::Unschedulable;
    std::string message;
    std::vector<AttemptRecord> attempts;
};

// All shared scheduling state lives here and is protected by `mu`.
class Run {
public:
    Run(const std::vector<AgentInfo>& agents, Executor& exec, const std::vector<Task>& tasks)
        : agents_(agents), exec_(exec), tasks_(tasks), st_(tasks.size()), alive_(agents.size(), true),
          remaining_(tasks.size()) {}

    RunSummary execute() {
        const auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mu_);
            resolve_unschedulable();
        }
        std::vector<std::thread> workers;
        for (size_t a = 0; a < agents_.size(); ++a) {
            for (unsigned s = 0; s < agents_[a].slots; ++s) workers.emplace_back([this, a] { worker(a); });
        }
        for (auto& w : workers) w.join();

        RunSummary sum;
        for (size_t i = 0; i < tasks_.size(); ++i) {
            TaskOutcome o;
            o.task = tasks_[i];
            o.status = st_[i].final;
            o.attempts = std::move(st_[i].attempts);
            o.message = st_[i].message;
            sum.outcomes.push_back(std::move(o));
        }
        sum.dead_agents = dead_;
        sum.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return sum;
    }

private:
    bool able(size_t agent, size_t task) const { return labels_satisfy(agents_[agent].labels, tasks_[task].needs); }

    // Is there another live, capable agent this task has not been tried on?
    bool has_fresh_candidate(size_t task, size_t except_agent) const {
        for (size_t a = 0; a < agents_.size(); ++a) {
            if (a != except_agent && alive_[a] && able(a, task) && !st_[task].tried.count(a)) return true;
        }
        return false;
    }

    bool pick(size_t agent, size_t* out) const {
        for (size_t t = 0; t < tasks_.size(); ++t) {
            const TaskState& s = st_[t];
            if (s.done || s.running || !able(agent, t)) continue;
            // Retry on a different agent when one is available, to escape a bad host.
            if (s.tried.count(agent) && has_fresh_candidate(t, agent)) continue;
            *out = t;
            return true;
        }
        return false;
    }

    void finish(size_t t, FinalStatus f, std::string msg = "") {
        st_[t].done = true;
        st_[t].final = f;
        st_[t].message = std::move(msg);
        --remaining_;
    }

    // Marks pending tasks that no live agent can ever run.
    void resolve_unschedulable() {
        for (size_t t = 0; t < tasks_.size(); ++t) {
            if (st_[t].done || st_[t].running) continue;
            bool any_capable = false, any_live_capable = false;
            for (size_t a = 0; a < agents_.size(); ++a) {
                if (!able(a, t)) continue;
                any_capable = true;
                if (alive_[a]) any_live_capable = true;
            }
            if (any_live_capable) continue;
            std::string req;
            for (const auto& r : tasks_[t].needs) req += (req.empty() ? "" : ", ") + r;
            finish(t, FinalStatus::Unschedulable,
                   any_capable ? "every agent that could run this task became unreachable"
                               : "no agent provides the required labels" + (req.empty() ? std::string() : " [" + req + "]"));
        }
    }

    void worker(size_t a) {
        std::unique_lock<std::mutex> lk(mu_);
        while (remaining_ > 0 && alive_[a]) {
            size_t t = 0;
            if (!pick(a, &t)) {
                cv_.wait(lk);
                continue;
            }
            st_[t].running = true;
            lk.unlock();
            AttemptResult res = exec_.run(agents_[a], tasks_[t]);
            lk.lock();

            TaskState& s = st_[t];
            s.running = false;
            s.tried.insert(a);
            s.attempts.push_back({agents_[a].address, res});

            switch (res.status) {
                case AttemptStatus::Passed:
                    finish(t, FinalStatus::Passed);
                    break;
                case AttemptStatus::Failed:
                case AttemptStatus::TimedOut:
                    ++s.failures;
                    if (s.failures > tasks_[t].retries) {
                        finish(t, res.status == AttemptStatus::TimedOut ? FinalStatus::TimedOut : FinalStatus::Failed);
                    }
                    break;
                case AttemptStatus::AgentError:
                    // Retire the agent; the task goes back to the queue for free. Several slots of the
                    // same agent can fail at once, so only the first one records the retirement.
                    if (alive_[a]) {
                        alive_[a] = false;
                        dead_.push_back(agents_[a].address);
                        resolve_unschedulable();
                    }
                    break;
            }
            cv_.notify_all();
        }
        cv_.notify_all();
    }

    const std::vector<AgentInfo>& agents_;
    Executor& exec_;
    const std::vector<Task>& tasks_;
    std::vector<TaskState> st_;
    std::vector<bool> alive_;
    std::vector<std::string> dead_;
    size_t remaining_;
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace

RunSummary Scheduler::run(const std::vector<Task>& tasks) {
    Run r(agents_, exec_, tasks);
    return r.execute();
}

}  // namespace dtest
