#include "dtest/report.hpp"

#include <iomanip>
#include <sstream>

namespace dtest {

std::string xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 forbids most control characters; replace them so the report stays parseable.
                if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') {
                    out += '?';
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string tail(const std::string& s, size_t max_lines, size_t max_bytes) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    size_t count = 0, pos = t.size();
    while (pos > 0) {
        if (t[pos - 1] == '\n' && ++count >= max_lines) break;
        --pos;
    }
    t = t.substr(pos);
    if (t.size() > max_bytes) t = t.substr(t.size() - max_bytes);
    return t;
}

namespace {

std::string describe_last_attempt(const TaskOutcome& o) {
    if (o.attempts.empty()) return o.message;
    const AttemptResult& r = o.attempts.back().result;
    std::ostringstream os;
    switch (r.status) {
        case AttemptStatus::Passed: os << "passed"; break;
        case AttemptStatus::Failed: os << (r.exit_code >= 0 ? "exit code " + std::to_string(r.exit_code) : "failed"); break;
        case AttemptStatus::TimedOut: os << "timed out"; break;
        case AttemptStatus::AgentError: os << "agent error"; break;
    }
    if (!r.message.empty()) os << " (" << r.message << ")";
    return os.str();
}

std::string last_agent(const TaskOutcome& o) { return o.attempts.empty() ? std::string("unscheduled") : o.attempts.back().agent; }

}  // namespace

std::string render_text(const RunSummary& s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    for (const auto& o : s.outcomes) {
        const char* tag = o.status == FinalStatus::Passed ? (o.flaky() ? "FLAKY" : "PASS")
                          : o.status == FinalStatus::TimedOut ? "TIMEOUT"
                          : o.status == FinalStatus::Unschedulable ? "SKIPPED" : "FAIL";
        os << "[" << tag << "] " << o.task.name;
        if (o.status == FinalStatus::Unschedulable) {
            os << " - " << o.message << "\n";
            continue;
        }
        os << " on " << last_agent(o) << " (" << o.attempts.back().result.seconds << "s";
        if (o.attempts.size() > 1) os << ", " << o.attempts.size() << " attempts";
        os << ")";
        if (o.status != FinalStatus::Passed) os << " - " << describe_last_attempt(o);
        os << "\n";
        if (o.status != FinalStatus::Passed) {
            const std::string t = tail(o.attempts.back().result.err.empty() ? o.attempts.back().result.out
                                                                             : o.attempts.back().result.err, 5, 800);
            if (!t.empty()) {
                std::istringstream in(t);
                std::string line;
                while (std::getline(in, line)) os << "        | " << line << "\n";
            }
        }
    }
    os << "\n" << s.outcomes.size() << " tasks: " << s.count(FinalStatus::Passed) << " passed";
    if (s.flaky_count()) os << " (" << s.flaky_count() << " flaky)";
    os << ", " << s.count(FinalStatus::Failed) << " failed, " << s.count(FinalStatus::TimedOut) << " timed out, "
       << s.count(FinalStatus::Unschedulable) << " unschedulable in " << s.wall_seconds << "s\n";
    if (!s.dead_agents.empty()) {
        os << "agents retired after infrastructure errors:";
        for (const auto& a : s.dead_agents) os << " " << a;
        os << "\n";
    }
    return os.str();
}

std::string render_junit(const std::string& suite_name, const RunSummary& s) {
    const int failures = s.count(FinalStatus::Failed) + s.count(FinalStatus::TimedOut);
    const int skipped = s.count(FinalStatus::Unschedulable);
    double total = 0;
    for (const auto& o : s.outcomes) {
        for (const auto& a : o.attempts) total += a.result.seconds;
    }

    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    os << "<testsuites name=\"" << xml_escape(suite_name) << "\" tests=\"" << s.outcomes.size() << "\" failures=\""
       << failures << "\" skipped=\"" << skipped << "\" time=\"" << total << "\">\n";
    os << "  <testsuite name=\"" << xml_escape(suite_name) << "\" tests=\"" << s.outcomes.size() << "\" failures=\""
       << failures << "\" skipped=\"" << skipped << "\" time=\"" << total << "\">\n";

    for (const auto& o : s.outcomes) {
        double secs = 0;
        for (const auto& a : o.attempts) secs += a.result.seconds;
        os << "    <testcase classname=\"" << xml_escape(last_agent(o)) << "\" name=\"" << xml_escape(o.task.name)
           << "\" time=\"" << secs << "\"";
        if (o.status == FinalStatus::Passed && !o.flaky()) {
            os << "/>\n";
            continue;
        }
        os << ">\n";
        if (o.status == FinalStatus::Unschedulable) {
            os << "      <skipped message=\"" << xml_escape(o.message) << "\"/>\n";
        } else if (o.status != FinalStatus::Passed) {
            os << "      <failure message=\"" << xml_escape(describe_last_attempt(o)) << "\">"
               << xml_escape(tail(o.attempts.back().result.err, 40, 4000)) << "</failure>\n";
        }
        if (o.flaky()) {
            os << "      <system-out>flaky: passed on attempt " << o.attempts.size() << " of " << o.attempts.size()
               << " (earlier attempts failed)</system-out>\n";
        }
        os << "    </testcase>\n";
    }
    os << "  </testsuite>\n</testsuites>\n";
    return os.str();
}

}  // namespace dtest
