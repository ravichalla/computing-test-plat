#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "dtest/report.hpp"
#include "dtest/scheduler.hpp"

using namespace dtest;
using namespace std::chrono_literals;

namespace {

class FakeExecutor final : public Executor {
public:
    std::function<AttemptResult(const AgentInfo&, const Task&)> fn;
    AttemptResult run(const AgentInfo& a, const Task& t) override { return fn(a, t); }
};

AttemptResult passed() {
    AttemptResult r;
    r.status = AttemptStatus::Passed;
    r.exit_code = 0;
    r.seconds = 0.01;
    return r;
}
AttemptResult failed(int code = 1) {
    AttemptResult r;
    r.status = AttemptStatus::Failed;
    r.exit_code = code;
    r.err = "boom\n";
    r.seconds = 0.01;
    return r;
}
AttemptResult timed_out() {
    AttemptResult r;
    r.status = AttemptStatus::TimedOut;
    r.message = "exceeded timeout";
    return r;
}
AttemptResult agent_error() {
    AttemptResult r;
    r.status = AttemptStatus::AgentError;
    r.message = "connection refused";
    return r;
}

AgentInfo agent(const std::string& addr, unsigned slots, std::vector<std::string> labels = {}) {
    AgentInfo a;
    a.address = addr;
    a.hostname = addr;
    a.slots = slots;
    a.labels = std::move(labels);
    return a;
}

Task task(const std::string& name, int retries = 0, std::vector<std::string> needs = {}) {
    Task t;
    t.name = name;
    t.argv = {"/bin/true"};
    t.retries = retries;
    t.needs = std::move(needs);
    return t;
}

std::vector<Task> tasks(int n, const std::string& prefix = "t", int retries = 0) {
    std::vector<Task> v;
    for (int i = 0; i < n; ++i) v.push_back(task(prefix + std::to_string(i), retries));
    return v;
}

// A scheduler bug would show up as a hang; make it a test failure instead.
RunSummary run_guarded(Scheduler& s, const std::vector<Task>& ts) {
    auto fut = std::async(std::launch::async, [&] { return s.run(ts); });
    if (fut.wait_for(20s) != std::future_status::ready) {
        ADD_FAILURE() << "scheduler did not terminate (deadlock)";
        std::abort();
    }
    return fut.get();
}

}  // namespace

TEST(Scheduler, RunsEverythingAndKeepsInputOrder) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return passed(); };
    Scheduler s({agent("A", 2), agent("B", 2)}, ex);
    const auto in = tasks(20);
    const auto sum = run_guarded(s, in);

    ASSERT_EQ(sum.outcomes.size(), 20u);
    for (size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(sum.outcomes[i].task.name, in[i].name);
        EXPECT_EQ(sum.outcomes[i].status, FinalStatus::Passed);
        EXPECT_EQ(sum.outcomes[i].attempts.size(), 1u);
    }
    EXPECT_TRUE(sum.all_passed());
    EXPECT_TRUE(sum.dead_agents.empty());
}

TEST(Scheduler, NeverExceedsAnAgentsSlotCount) {
    std::mutex mu;
    std::map<std::string, int> cur, peak;
    FakeExecutor ex;
    ex.fn = [&](const AgentInfo& a, const Task&) {
        {
            std::lock_guard<std::mutex> lk(mu);
            peak[a.address] = std::max(peak[a.address], ++cur[a.address]);
        }
        std::this_thread::sleep_for(15ms);
        {
            std::lock_guard<std::mutex> lk(mu);
            --cur[a.address];
        }
        return passed();
    };
    Scheduler s({agent("A", 1), agent("B", 3)}, ex);
    run_guarded(s, tasks(30));
    EXPECT_LE(peak["A"], 1);
    EXPECT_LE(peak["B"], 3);
    EXPECT_GE(peak["B"], 2) << "expected the 3-slot agent to actually run tasks in parallel";
}

TEST(Scheduler, TasksOnlyRunOnAgentsWithTheRequiredLabels) {
    std::mutex mu;
    std::map<std::string, std::string> ran_on;
    FakeExecutor ex;
    ex.fn = [&](const AgentInfo& a, const Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        ran_on[t.name] = a.address;
        return passed();
    };
    Scheduler s({agent("plain", 2, {"os=linux"}), agent("kvmhost", 2, {"os=linux", "kvm=true"})}, ex);
    std::vector<Task> ts;
    for (int i = 0; i < 10; ++i) ts.push_back(task("vm" + std::to_string(i), 0, {"kvm=true"}));
    for (int i = 0; i < 10; ++i) ts.push_back(task("any" + std::to_string(i)));
    const auto sum = run_guarded(s, ts);

    EXPECT_TRUE(sum.all_passed());
    for (int i = 0; i < 10; ++i) EXPECT_EQ(ran_on["vm" + std::to_string(i)], "kvmhost");
}

TEST(Scheduler, TaskNoAgentCanRunIsUnschedulableAndDoesNotBlockOthers) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return passed(); };
    Scheduler s({agent("A", 2, {"os=linux"})}, ex);
    const auto sum = run_guarded(s, {task("ok"), task("needs-gpu", 0, {"gpu=true"}), task("ok2")});

    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Passed);
    EXPECT_EQ(sum.outcomes[2].status, FinalStatus::Passed);
    EXPECT_EQ(sum.outcomes[1].status, FinalStatus::Unschedulable);
    EXPECT_NE(sum.outcomes[1].message.find("gpu=true"), std::string::npos) << sum.outcomes[1].message;
    EXPECT_TRUE(sum.outcomes[1].attempts.empty());
    EXPECT_FALSE(sum.all_passed());
}

TEST(Scheduler, NoAgentsMeansEverythingIsUnschedulable) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return passed(); };
    Scheduler s({}, ex);
    const auto sum = run_guarded(s, tasks(3));
    EXPECT_EQ(sum.count(FinalStatus::Unschedulable), 3);
}

TEST(Scheduler, FailedTestsUseUpRetriesThenFail) {
    std::atomic<int> calls{0};
    FakeExecutor ex;
    ex.fn = [&](const AgentInfo&, const Task&) {
        ++calls;
        return failed(2);
    };
    Scheduler s({agent("A", 1)}, ex);
    const auto sum = run_guarded(s, {task("always-fails", /*retries=*/2)});

    EXPECT_EQ(calls.load(), 3);  // 1 attempt + 2 retries
    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Failed);
    EXPECT_EQ(sum.outcomes[0].attempts.size(), 3u);
    EXPECT_FALSE(sum.outcomes[0].flaky());
}

TEST(Scheduler, ATimeoutIsReportedAsTimedOut) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return timed_out(); };
    Scheduler s({agent("A", 1)}, ex);
    const auto sum = run_guarded(s, {task("slow")});
    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::TimedOut);
}

TEST(Scheduler, PassingAfterAFailureIsFlaggedFlaky) {
    std::mutex mu;
    std::map<std::string, int> seen;
    FakeExecutor ex;
    ex.fn = [&](const AgentInfo&, const Task& t) {
        if (t.name == "solid") return passed();
        std::lock_guard<std::mutex> lk(mu);
        return ++seen[t.name] == 1 ? failed() : passed();  // "flaky" fails once, then passes
    };
    Scheduler s({agent("A", 1)}, ex);
    const auto sum = run_guarded(s, {task("flaky", 1), task("solid", 1)});

    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Passed);
    EXPECT_TRUE(sum.outcomes[0].flaky());
    EXPECT_EQ(sum.outcomes[0].attempts.size(), 2u);
    EXPECT_FALSE(sum.outcomes[1].flaky());
    EXPECT_EQ(sum.outcomes[1].attempts.size(), 1u);
    EXPECT_EQ(sum.flaky_count(), 1);
    EXPECT_TRUE(sum.all_passed());  // flaky still passes, but is visible
}

TEST(Scheduler, RetriesAvoidTheAgentThatJustFailedTheTask) {
    // Agent "bad" fails everything; "good" passes. With one retry every task must end up
    // passing, and any task that first landed on "bad" must have been retried on "good".
    FakeExecutor ex;
    ex.fn = [](const AgentInfo& a, const Task&) { return a.address == "bad" ? failed() : passed(); };
    Scheduler s({agent("bad", 2), agent("good", 2)}, ex);
    const auto sum = run_guarded(s, tasks(20, "t", /*retries=*/1));

    for (const auto& o : sum.outcomes) {
        EXPECT_EQ(o.status, FinalStatus::Passed) << o.task.name;
        if (o.attempts.size() == 2) {
            EXPECT_EQ(o.attempts[0].agent, "bad");
            EXPECT_EQ(o.attempts[1].agent, "good");
        }
    }
}

TEST(Scheduler, AnUnreachableAgentIsRetiredAndItsTasksAreRescheduledForFree) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo& a, const Task&) { return a.address == "down" ? agent_error() : passed(); };
    Scheduler s({agent("down", 2), agent("up", 2)}, ex);
    // retries=0: if an outage consumed a retry budget these tasks would fail.
    const auto sum = run_guarded(s, tasks(20, "t", /*retries=*/0));

    EXPECT_TRUE(sum.all_passed());
    ASSERT_EQ(sum.dead_agents.size(), 1u);
    EXPECT_EQ(sum.dead_agents[0], "down");
    for (const auto& o : sum.outcomes) EXPECT_EQ(o.attempts.back().agent, "up");
}

TEST(Scheduler, SeveralSlotsFailingAtOnceRetireTheAgentOnlyOnce) {
    // Force both slots of "down" to be in flight together, then fail together.
    std::mutex mu;
    std::condition_variable cv;
    int arrived = 0;
    FakeExecutor ex;
    ex.fn = [&](const AgentInfo& a, const Task&) {
        if (a.address != "down") return passed();
        std::unique_lock<std::mutex> lk(mu);
        ++arrived;
        cv.notify_all();
        cv.wait_for(lk, 5s, [&] { return arrived >= 2; });
        return agent_error();
    };
    Scheduler s({agent("down", 2), agent("up", 1)}, ex);
    const auto sum = run_guarded(s, tasks(6));

    EXPECT_TRUE(sum.all_passed());
    EXPECT_EQ(sum.dead_agents, std::vector<std::string>{"down"});
}

TEST(Scheduler, WhenEveryAgentDiesRemainingTasksBecomeUnschedulableInsteadOfHanging) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return agent_error(); };
    Scheduler s({agent("A", 2), agent("B", 2)}, ex);
    const auto sum = run_guarded(s, tasks(10));

    EXPECT_EQ(sum.count(FinalStatus::Unschedulable), 10);
    EXPECT_EQ(sum.dead_agents.size(), 2u);
    for (const auto& o : sum.outcomes) {
        EXPECT_NE(o.message.find("unreachable"), std::string::npos) << o.message;
    }
}

TEST(Scheduler, ALabelSpecificAgentDyingStrandsOnlyTheTasksThatNeedIt) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo& a, const Task&) { return a.address == "kvmhost" ? agent_error() : passed(); };
    Scheduler s({agent("kvmhost", 1, {"kvm=true"}), agent("plain", 2)}, ex);
    const auto sum = run_guarded(s, {task("needs-kvm", 0, {"kvm=true"}), task("generic-1"), task("generic-2")});

    EXPECT_EQ(sum.outcomes[0].status, FinalStatus::Unschedulable);
    EXPECT_EQ(sum.outcomes[1].status, FinalStatus::Passed);
    EXPECT_EQ(sum.outcomes[2].status, FinalStatus::Passed);
}

TEST(Scheduler, ZeroSlotsIsTreatedAsOne) {
    FakeExecutor ex;
    ex.fn = [](const AgentInfo&, const Task&) { return passed(); };
    Scheduler s({agent("A", 0)}, ex);
    EXPECT_TRUE(run_guarded(s, tasks(3)).all_passed());
}

// ---- reports ----------------------------------------------------------------

TEST(Report, TailKeepsTheEndOfTheOutput) {
    EXPECT_EQ(tail("a\nb\nc\nd\n", 2, 1000), "c\nd");
    EXPECT_EQ(tail("a\nb", 5, 1000), "a\nb");
    EXPECT_EQ(tail("one\n", 1, 1000), "one");
    EXPECT_EQ(tail("abcdefghij", 10, 4), "ghij");
    EXPECT_EQ(tail("", 3, 10), "");
}

TEST(Report, XmlEscapeNeutralisesControlCharacters) {
    EXPECT_EQ(xml_escape("a<b>&\"'"), "a&lt;b&gt;&amp;&quot;&apos;");
    EXPECT_EQ(xml_escape(std::string("x\x01y")), "x?y");
    EXPECT_EQ(xml_escape("tab\tnl\n"), "tab\tnl\n");
}

TEST(Report, TextAndJunitDescribeEachOutcomeKind) {
    FakeExecutor ex;
    std::mutex mu;
    std::map<std::string, int> seen;
    ex.fn = [&](const AgentInfo&, const Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        if (t.name == "pass") return passed();
        if (t.name == "flaky") return ++seen[t.name] == 1 ? failed() : passed();
        if (t.name == "fail") return failed(3);
        return timed_out();
    };
    Scheduler s({agent("A", 1)}, ex);
    const auto sum = run_guarded(s, {task("pass"), task("flaky", 1), task("fail"), task("slow"), task("gpu", 0, {"gpu"})});

    const std::string text = render_text(sum);
    EXPECT_NE(text.find("[PASS] pass"), std::string::npos) << text;
    EXPECT_NE(text.find("[FLAKY] flaky"), std::string::npos) << text;
    EXPECT_NE(text.find("[FAIL] fail"), std::string::npos) << text;
    EXPECT_NE(text.find("exit code 3"), std::string::npos) << text;
    EXPECT_NE(text.find("| boom"), std::string::npos) << text;  // stderr tail is shown
    EXPECT_NE(text.find("[TIMEOUT] slow"), std::string::npos) << text;
    EXPECT_NE(text.find("[SKIPPED] gpu"), std::string::npos) << text;
    EXPECT_NE(text.find("5 tasks: 2 passed (1 flaky), 1 failed, 1 timed out, 1 unschedulable"), std::string::npos) << text;

    const std::string xml = render_junit("nightly <x>", sum);
    EXPECT_NE(xml.find("<testsuites name=\"nightly &lt;x&gt;\" tests=\"5\" failures=\"2\" skipped=\"1\""), std::string::npos) << xml;
    EXPECT_NE(xml.find("<failure message=\"exit code 3\">boom</failure>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<skipped message=\"no agent provides the required labels [gpu]\"/>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<system-out>flaky:"), std::string::npos) << xml;
}
