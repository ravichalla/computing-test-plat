// dtest-ctl: schedules a job file across dtest agents and reports the results.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "dtest/grpc_executor.hpp"
#include "dtest/jobs.hpp"
#include "dtest/report.hpp"
#include "dtest/scheduler.hpp"

namespace {

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --jobs FILE --agent HOST:PORT [--agent HOST:PORT ...] [options]\n\n"
        << "Options:\n"
        << "  --jobs FILE            job file (JSON), see dtest/examples/jobs.json\n"
        << "  --agent HOST:PORT      an agent to use; repeatable\n"
        << "  --token TOKEN          shared secret (or set DTEST_TOKEN)\n"
        << "  --junit FILE           also write JUnit XML\n"
        << "  --connect-timeout S    seconds to wait for each agent at startup (default 5)\n"
        << "  --validate             only check the job file and exit\n"
        << "  -h, --help             show this help\n\n"
        << "Exit status: 0 all tasks passed, 1 a task failed / timed out / could not be scheduled, 2 usage error.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string jobs_path, junit_path, token;
    std::vector<std::string> agent_addrs;
    int connect_timeout = 5;
    bool validate_only = false;

    if (const char* env = std::getenv("DTEST_TOKEN")) token = env;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--jobs") {
            if (!val(&jobs_path)) { std::cerr << "--jobs needs a value\n"; return 2; }
        } else if (a == "--agent") {
            if (!val(&v)) { std::cerr << "--agent needs a value\n"; return 2; }
            agent_addrs.push_back(v);
        } else if (a == "--token") {
            if (!val(&token)) { std::cerr << "--token needs a value\n"; return 2; }
        } else if (a == "--junit") {
            if (!val(&junit_path)) { std::cerr << "--junit needs a value\n"; return 2; }
        } else if (a == "--connect-timeout") {
            if (!val(&v) || std::atoi(v.c_str()) < 1) { std::cerr << "--connect-timeout needs a positive integer\n"; return 2; }
            connect_timeout = std::atoi(v.c_str());
        } else if (a == "--validate") {
            validate_only = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            usage(argv[0]);
            return 2;
        }
    }
    if (jobs_path.empty()) { std::cerr << "error: --jobs is required\n\n"; usage(argv[0]); return 2; }
    if (agent_addrs.empty() && !validate_only) { std::cerr << "error: at least one --agent is required\n\n"; usage(argv[0]); return 2; }

    std::ifstream in(jobs_path);
    if (!in) { std::cerr << "error: cannot read " << jobs_path << "\n"; return 2; }
    std::stringstream buf;
    buf << in.rdbuf();
    const dtest::JobParseResult parsed = dtest::parse_jobs(buf.str());
    if (!parsed.error.empty()) { std::cerr << "error: " << jobs_path << ": " << parsed.error << "\n"; return 2; }
    std::cout << "job '" << parsed.job.name << "': " << parsed.job.tasks.size() << " tasks\n";
    if (validate_only) return 0;

    // Connect to all agents in parallel.
    dtest::GrpcExecutor executor(token);
    std::vector<dtest::AgentInfo> agents(agent_addrs.size());
    std::vector<std::string> errors(agent_addrs.size());
    std::vector<char> ok(agent_addrs.size(), 0);
    {
        std::vector<std::thread> threads;
        for (size_t i = 0; i < agent_addrs.size(); ++i) {
            threads.emplace_back([&, i] {
                ok[i] = executor.connect(agent_addrs[i], std::chrono::seconds(connect_timeout), &agents[i], &errors[i]) ? 1 : 0;
            });
        }
        for (auto& t : threads) t.join();
    }

    std::vector<dtest::AgentInfo> live;
    for (size_t i = 0; i < agent_addrs.size(); ++i) {
        if (!ok[i]) {
            std::cerr << "warning: agent " << agent_addrs[i] << " " << errors[i] << "\n";
            continue;
        }
        std::cout << "agent " << agents[i].address << " host=" << agents[i].hostname << " slots=" << agents[i].slots << " labels=";
        for (size_t l = 0; l < agents[i].labels.size(); ++l) std::cout << (l ? "," : "") << agents[i].labels[l];
        std::cout << "\n";
        live.push_back(agents[i]);
    }
    if (live.empty()) { std::cerr << "error: no agent is reachable\n"; return 1; }
    std::cout << "\n";

    dtest::Scheduler scheduler(live, executor);
    const dtest::RunSummary summary = scheduler.run(parsed.job.tasks);

    std::cout << dtest::render_text(summary);
    if (!junit_path.empty()) {
        std::ofstream out(junit_path);
        if (!out) { std::cerr << "error: cannot write " << junit_path << "\n"; return 2; }
        out << dtest::render_junit(parsed.job.name, summary);
        std::cout << "JUnit report written to " << junit_path << "\n";
    }
    return summary.all_passed() ? 0 : 1;
}
