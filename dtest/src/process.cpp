#include "dtest/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

namespace dtest {

namespace {

using Clock = std::chrono::steady_clock;

void set_nonblocking_cloexec(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Reads whatever is available on fd. Returns false once the stream is at EOF.
// Bytes beyond `limit` are discarded and `truncated` is set.
bool drain(int fd, std::string& into, size_t limit, bool& truncated) {
    char buf[4096];
    while (true) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            const size_t room = limit > into.size() ? limit - into.size() : 0;
            const size_t take = std::min(static_cast<size_t>(n), room);
            into.append(buf, take);
            if (take < static_cast<size_t>(n)) truncated = true;
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;  // no data right now
    }
}

}  // namespace

std::string to_string(RunOutcome o) {
    switch (o) {
        case RunOutcome::Exited: return "exited";
        case RunOutcome::Signaled: return "signaled";
        case RunOutcome::TimedOut: return "timed-out";
        case RunOutcome::LaunchFailed: return "launch-failed";
    }
    return "?";
}

RunOutput run_process(const RunSpec& spec) {
    RunOutput r;
    const auto start = Clock::now();
    auto finish = [&](RunOutput&& o) {
        o.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        return std::move(o);
    };

    if (spec.argv.empty() || spec.argv[0].empty()) {
        r.message = "empty argv";
        return finish(std::move(r));
    }

    int out_pipe[2], err_pipe[2], exec_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(exec_pipe) != 0) {
        r.message = std::string("pipe failed: ") + std::strerror(errno);
        return finish(std::move(r));
    }
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);  // closes on successful exec => parent sees EOF

    // Build argv/envp before fork (no allocation in the child).
    std::vector<char*> argv;
    for (const auto& a : spec.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::map<std::string, std::string> env = {{"PATH", "/usr/local/bin:/usr/bin:/bin"}, {"LC_ALL", "C"}};
    for (const auto& [k, v] : spec.env) env[k] = v;
    std::vector<std::string> env_strs;
    for (const auto& [k, v] : env) env_strs.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strs) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.message = std::string("fork failed: ") + std::strerror(errno);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], exec_pipe[0], exec_pipe[1]}) close(fd);
        return finish(std::move(r));
    }

    if (pid == 0) {
        setsid();  // own process group so a timeout can kill the whole tree
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(err_pipe[0]);
        close(exec_pipe[0]);
        execvpe(argv[0], argv.data(), envp.data());
        const int e = errno;
        (void)!write(exec_pipe[1], &e, sizeof e);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    close(exec_pipe[1]);

    // Did exec succeed? EOF with no data means yes.
    int exec_errno = 0;
    ssize_t n;
    do {
        n = read(exec_pipe[0], &exec_errno, sizeof exec_errno);
    } while (n < 0 && errno == EINTR);
    close(exec_pipe[0]);
    if (n == static_cast<ssize_t>(sizeof exec_errno)) {
        waitpid(pid, nullptr, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        r.outcome = RunOutcome::LaunchFailed;
        r.message = "cannot execute '" + spec.argv[0] + "': " + std::strerror(exec_errno);
        return finish(std::move(r));
    }

    set_nonblocking_cloexec(out_pipe[0]);
    set_nonblocking_cloexec(err_pipe[0]);

    const auto deadline = start + spec.timeout;
    bool out_open = true, err_open = true;
    bool timed_out = false;
    int status = 0;

    auto read_ready = [&](int poll_ms) {
        pollfd fds[2];
        int nfds = 0;
        if (out_open) fds[nfds++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[nfds++] = {err_pipe[0], POLLIN, 0};
        if (nfds == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        } else {
            poll(fds, nfds, poll_ms);
        }
        if (out_open) out_open = drain(out_pipe[0], r.out, spec.max_output_bytes, r.truncated);
        if (err_open) err_open = drain(err_pipe[0], r.err, spec.max_output_bytes, r.truncated);
    };

    // Detect exit WITHOUT reaping (WNOWAIT): the zombie leader keeps the process-group id
    // reserved, so the sweep below can never hit an unrelated group after pid reuse.
    auto has_exited = [&] {
        siginfo_t si;
        std::memset(&si, 0, sizeof si);
        return waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid;
    };

    while (true) {
        read_ready(20);
        if (has_exited()) break;
        if (Clock::now() >= deadline) {
            timed_out = true;
            break;
        }
    }

    if (timed_out) {
        killpg(pid, SIGTERM);  // polite first
        const auto grace = Clock::now() + std::chrono::milliseconds(1000);
        while (Clock::now() < grace) {
            read_ready(20);
            if (has_exited()) break;
        }
    }
    // Kill anything left in the group (the main process if it ignored SIGTERM, or
    // background children that outlived it), then reap.
    killpg(pid, SIGKILL);
    waitpid(pid, &status, 0);

    // Final non-blocking drain: pick up output written just before exit.
    if (out_open) drain(out_pipe[0], r.out, spec.max_output_bytes, r.truncated);
    if (err_open) drain(err_pipe[0], r.err, spec.max_output_bytes, r.truncated);
    close(out_pipe[0]);
    close(err_pipe[0]);

    if (timed_out) {
        r.outcome = RunOutcome::TimedOut;
        r.message = "exceeded timeout of " + std::to_string(spec.timeout.count()) + " ms";
    } else if (WIFEXITED(status)) {
        r.outcome = RunOutcome::Exited;
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.outcome = RunOutcome::Signaled;
        r.term_signal = WTERMSIG(status);
    }
    return finish(std::move(r));
}

}  // namespace dtest
