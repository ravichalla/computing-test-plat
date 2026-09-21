#pragma once

#include <map>
#include <string>
#include <vector>

namespace dtest {

struct Task {
    std::string name;
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    int timeout_s = 60;
    int retries = 0;                    // extra attempts after a failure (not after an agent outage)
    std::vector<std::string> needs;  // agent labels needed: "key=value", or "key" for any value
};

struct JobFile {
    std::string name = "dtest";
    std::vector<Task> tasks;  // after "repeat" expansion
};

struct JobParseResult {
    JobFile job;
    std::string error;  // empty on success
};

// Parses and validates a job file. Unknown keys are rejected so typos such as
// "retires" fail loudly instead of silently doing nothing.
JobParseResult parse_jobs(const std::string& json_text);

// True if `agent_labels` satisfies every requirement.
bool labels_satisfy(const std::vector<std::string>& agent_labels, const std::vector<std::string>& needs);

}  // namespace dtest
