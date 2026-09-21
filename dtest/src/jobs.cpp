#include "dtest/jobs.hpp"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

namespace dtest {

using nlohmann::json;

bool labels_satisfy(const std::vector<std::string>& agent_labels, const std::vector<std::string>& needs) {
    for (const auto& req : needs) {
        const bool has_value = req.find('=') != std::string::npos;
        bool found = false;
        for (const auto& l : agent_labels) {
            if (has_value ? (l == req) : (l == req || l.rfind(req + "=", 0) == 0)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

namespace {

constexpr size_t kMaxTasks = 100000;


bool is_int_in(const json& v, long lo, long hi, long* out) {
    if (!v.is_number_integer()) return false;
    const long x = v.get<long>();
    if (x < lo || x > hi) return false;
    *out = x;
    return true;
}

std::string where(size_t i, const json& t) {
    std::string w = "tasks[" + std::to_string(i) + "]";
    if (t.is_object() && t.contains("name") && t["name"].is_string()) w += " ('" + t["name"].get<std::string>() + "')";
    return w;
}

bool read_string_array(const json& v, std::vector<std::string>* out) {
    if (!v.is_array()) return false;
    for (const auto& e : v) {
        if (!e.is_string()) return false;
        out->push_back(e.get<std::string>());
    }
    return true;
}

bool read_env(const json& v, std::map<std::string, std::string>* out) {
    if (!v.is_object()) return false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (!it.value().is_string() || it.key().empty() || it.key().find('=') != std::string::npos) return false;
        (*out)[it.key()] = it.value().get<std::string>();
    }
    return true;
}

bool check_keys(const json& obj, const std::set<std::string>& allowed, std::string* bad) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!allowed.count(it.key())) {
            *bad = it.key();
            return false;
        }
    }
    return true;
}

}  // namespace

JobParseResult parse_jobs(const std::string& text) {
    JobParseResult res;
    auto fail = [&](const std::string& m) {
        res.error = m;
        res.job.tasks.clear();
        return res;
    };

    json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return fail("job file is not valid JSON");
    if (!root.is_object()) return fail("job file must be a JSON object");

    std::string bad;
    if (!check_keys(root, {"name", "defaults", "tasks"}, &bad)) return fail("unknown top-level key '" + bad + "'");

    if (root.contains("name")) {
        if (!root["name"].is_string() || root["name"].get<std::string>().empty()) return fail("'name' must be a non-empty string");
        res.job.name = root["name"].get<std::string>();
    }

    // defaults
    Task defaults;
    if (root.contains("defaults")) {
        const json& d = root["defaults"];
        if (!d.is_object()) return fail("'defaults' must be an object");
        if (!check_keys(d, {"timeout_s", "retries", "env", "requires"}, &bad)) return fail("unknown key '" + bad + "' in 'defaults'");
        long n = 0;
        if (d.contains("timeout_s")) {
            if (!is_int_in(d["timeout_s"], 1, 86400, &n)) return fail("'defaults.timeout_s' must be an integer in 1..86400");
            defaults.timeout_s = static_cast<int>(n);
        }
        if (d.contains("retries")) {
            if (!is_int_in(d["retries"], 0, 10, &n)) return fail("'defaults.retries' must be an integer in 0..10");
            defaults.retries = static_cast<int>(n);
        }
        if (d.contains("env") && !read_env(d["env"], &defaults.env)) return fail("'defaults.env' must be an object of string values");
        if (d.contains("requires") && !read_string_array(d["requires"], &defaults.needs)) return fail("'defaults.requires' must be an array of strings");
    }

    if (!root.contains("tasks") || !root["tasks"].is_array() || root["tasks"].empty()) {
        return fail("'tasks' must be a non-empty array");
    }

    std::set<std::string> names;
    size_t idx = 0;
    for (const json& t : root["tasks"]) {
        const std::string w = where(idx, t);
        if (!t.is_object()) return fail(w + ": must be an object");
        if (!check_keys(t, {"name", "argv", "env", "timeout_s", "retries", "requires", "repeat"}, &bad)) {
            return fail(w + ": unknown key '" + bad + "'");
        }

        Task task = defaults;
        if (!t.contains("name") || !t["name"].is_string() || t["name"].get<std::string>().empty()) {
            return fail(w + ": 'name' must be a non-empty string");
        }
        task.name = t["name"].get<std::string>();
        for (unsigned char c : task.name) {
            if (c < 0x20) return fail(w + ": 'name' contains control characters");
        }

        if (!t.contains("argv") || !read_string_array(t["argv"], &task.argv) || task.argv.empty() || task.argv[0].empty()) {
            return fail(w + ": 'argv' must be a non-empty array of strings with a non-empty first element");
        }

        long n = 0;
        if (t.contains("timeout_s")) {
            if (!is_int_in(t["timeout_s"], 1, 86400, &n)) return fail(w + ": 'timeout_s' must be an integer in 1..86400");
            task.timeout_s = static_cast<int>(n);
        }
        if (t.contains("retries")) {
            if (!is_int_in(t["retries"], 0, 10, &n)) return fail(w + ": 'retries' must be an integer in 0..10");
            task.retries = static_cast<int>(n);
        }
        if (t.contains("env")) {
            auto merged = defaults.env;
            std::map<std::string, std::string> own;
            if (!read_env(t["env"], &own)) return fail(w + ": 'env' must be an object of string values");
            for (auto& [k, v] : own) merged[k] = v;
            task.env = std::move(merged);
        }
        if (t.contains("requires")) {
            task.needs.clear();
            if (!read_string_array(t["requires"], &task.needs)) return fail(w + ": 'requires' must be an array of strings");
        }

        long repeat = 1;
        if (t.contains("repeat") && !is_int_in(t["repeat"], 1, 1000, &repeat)) {
            return fail(w + ": 'repeat' must be an integer in 1..1000");
        }

        for (long i = 1; i <= repeat; ++i) {
            Task copy = task;
            if (repeat > 1) copy.name = task.name + "[" + std::to_string(i) + "]";
            if (!names.insert(copy.name).second) return fail(w + ": duplicate task name '" + copy.name + "'");
            res.job.tasks.push_back(std::move(copy));
            if (res.job.tasks.size() > kMaxTasks) return fail("too many tasks (limit " + std::to_string(kMaxTasks) + ")");
        }
        ++idx;
    }
    return res;
}

}  // namespace dtest
