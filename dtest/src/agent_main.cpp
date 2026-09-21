// dtest-agent: runs commands on behalf of a dtest controller.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "dtest/agent_service.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n\n"
        << "Runs commands for a dtest controller. Because that is remote code execution by design, the\n"
        << "agent refuses to start without BOTH an authentication choice and a command policy.\n\n"
        << "Options:\n"
        << "  --listen ADDR      address to bind (default 127.0.0.1:7001; use :0 for any free port)\n"
        << "  --token TOKEN      shared secret the controller must present (or set DTEST_TOKEN)\n"
        << "  --no-auth          run without authentication (only for local experiments)\n"
        << "  --allow PATH       allow one program (absolute path) or a directory (trailing '/'); repeatable\n"
        << "  --allow-any        allow any command (dangerous; only for throwaway environments)\n"
        << "  --slots N          concurrent tasks (default: number of CPUs)\n"
        << "  --label K=V        extra capability label; repeatable (os, arch and kvm are detected)\n"
        << "  -h, --help         show this help\n\n"
        << "Traffic is plaintext gRPC: bind to loopback, or run on a trusted network / through a tunnel.\n";
}

bool is_loopback(const std::string& addr) {
    return addr.rfind("127.", 0) == 0 || addr.rfind("localhost:", 0) == 0 || addr.rfind("[::1]:", 0) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string listen = "127.0.0.1:7001";
    dtest::AgentService::Options opts;
    bool no_auth = false;
    unsigned slots = 0;
    std::vector<std::string> extra_labels;

    if (const char* env = std::getenv("DTEST_TOKEN")) opts.token = env;

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
        } else if (a == "--listen") {
            if (!val(&listen)) { std::cerr << "--listen needs a value\n"; return 2; }
        } else if (a == "--token") {
            if (!val(&opts.token)) { std::cerr << "--token needs a value\n"; return 2; }
        } else if (a == "--no-auth") {
            no_auth = true;
        } else if (a == "--allow") {
            if (!val(&v)) { std::cerr << "--allow needs a value\n"; return 2; }
            opts.policy.allow.push_back(v);
        } else if (a == "--allow-any") {
            opts.policy.allow_any = true;
        } else if (a == "--slots") {
            if (!val(&v) || std::atoi(v.c_str()) < 1) { std::cerr << "--slots needs a positive integer\n"; return 2; }
            slots = static_cast<unsigned>(std::atoi(v.c_str()));
        } else if (a == "--label") {
            if (!val(&v) || v.find('=') == std::string::npos) { std::cerr << "--label needs K=V\n"; return 2; }
            extra_labels.push_back(v);
        } else {
            std::cerr << "unknown option: " << a << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (opts.token.empty() && !no_auth) {
        std::cerr << "error: refusing to start without authentication. Pass --token (or set DTEST_TOKEN), "
                     "or --no-auth for local experiments.\n";
        return 2;
    }
    if (!opts.token.empty() && no_auth) {
        std::cerr << "error: --token and --no-auth are contradictory\n";
        return 2;
    }
    if (opts.policy.allow.empty() && !opts.policy.allow_any) {
        std::cerr << "error: refusing to start with no command policy. Pass --allow PATH (repeatable) or --allow-any.\n";
        return 2;
    }

    opts.slots = slots ? slots : std::max(1u, std::thread::hardware_concurrency());
    opts.labels = dtest::detect_labels();
    opts.labels.insert(opts.labels.end(), extra_labels.begin(), extra_labels.end());

    dtest::AgentService service(opts);

    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server || port == 0) {
        std::cerr << "error: could not listen on " << listen << "\n";
        return 1;
    }

    if (!is_loopback(listen)) {
        std::cerr << "WARNING: listening on a non-loopback address with plaintext gRPC. Use a trusted network or a tunnel.\n";
    }
    if (opts.policy.allow_any) std::cerr << "WARNING: --allow-any lets the controller run ANY command as this user.\n";
    if (no_auth) std::cerr << "WARNING: authentication is disabled.\n";

    // Report the actual port (useful with :0). Scripts wait for this line.
    const std::string host = listen.substr(0, listen.rfind(':'));
    std::cout << "dtest-agent listening on " << host << ":" << port << " slots=" << opts.slots << " labels=";
    for (size_t i = 0; i < opts.labels.size(); ++i) std::cout << (i ? "," : "") << opts.labels[i];
    std::cout << std::endl;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    std::cout << "dtest-agent stopped" << std::endl;
    return 0;
}
