#include <gtest/gtest.h>

#include "dtest/jobs.hpp"

using namespace dtest;

namespace {
JobParseResult parse(const std::string& s) { return parse_jobs(s); }
}  // namespace

TEST(Jobs, ParsesAMinimalFile) {
    const auto r = parse(R"({"tasks":[{"name":"a","argv":["/bin/true"]}]})");
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.job.tasks.size(), 1u);
    EXPECT_EQ(r.job.tasks[0].name, "a");
    EXPECT_EQ(r.job.tasks[0].timeout_s, 60);  // built-in default
    EXPECT_EQ(r.job.tasks[0].retries, 0);
}

TEST(Jobs, DefaultsApplyAndTasksOverrideThem) {
    const auto r = parse(R"({
      "name": "nightly",
      "defaults": {"timeout_s": 30, "retries": 2, "env": {"A":"1","B":"2"}, "requires": ["os=linux"]},
      "tasks": [
        {"name": "inherits", "argv": ["/bin/true"]},
        {"name": "overrides", "argv": ["/bin/true"], "timeout_s": 5, "retries": 0,
         "env": {"B":"3"}, "requires": ["kvm=true"]}
      ]})");
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(r.job.name, "nightly");
    const auto& a = r.job.tasks[0];
    EXPECT_EQ(a.timeout_s, 30);
    EXPECT_EQ(a.retries, 2);
    EXPECT_EQ(a.env.at("B"), "2");
    EXPECT_EQ(a.needs, std::vector<std::string>{"os=linux"});
    const auto& b = r.job.tasks[1];
    EXPECT_EQ(b.timeout_s, 5);
    EXPECT_EQ(b.retries, 0);
    EXPECT_EQ(b.env.at("A"), "1");  // env is merged, not replaced
    EXPECT_EQ(b.env.at("B"), "3");
    EXPECT_EQ(b.needs, std::vector<std::string>{"kvm=true"});  // needs is replaced
}

TEST(Jobs, RepeatExpandsIntoNumberedTasks) {
    const auto r = parse(R"({"tasks":[{"name":"soak","argv":["/bin/true"],"repeat":3}]})");
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.job.tasks.size(), 3u);
    EXPECT_EQ(r.job.tasks[0].name, "soak[1]");
    EXPECT_EQ(r.job.tasks[2].name, "soak[3]");
}

struct BadCase {
    const char* what;
    const char* json;
    const char* expect;  // substring of the error
};

TEST(Jobs, RejectsInvalidInputWithHelpfulMessages) {
    const BadCase cases[] = {
        {"not json", "{nope", "not valid JSON"},
        {"not an object", "[]", "must be a JSON object"},
        {"unknown top-level key", R"({"tasks":[{"name":"a","argv":["x"]}],"taskz":1})", "unknown top-level key 'taskz'"},
        {"no tasks", R"({"tasks":[]})", "non-empty array"},
        {"missing tasks", R"({})", "non-empty array"},
        {"typo'd task key", R"({"tasks":[{"name":"a","argv":["x"],"retires":1}]})", "unknown key 'retires'"},
        {"missing name", R"({"tasks":[{"argv":["x"]}]})", "'name'"},
        {"empty argv", R"({"tasks":[{"name":"a","argv":[]}]})", "'argv'"},
        {"empty argv[0]", R"({"tasks":[{"name":"a","argv":[""]}]})", "'argv'"},
        {"argv not strings", R"({"tasks":[{"name":"a","argv":[1]}]})", "'argv'"},
        {"zero timeout", R"({"tasks":[{"name":"a","argv":["x"],"timeout_s":0}]})", "timeout_s"},
        {"huge timeout", R"({"tasks":[{"name":"a","argv":["x"],"timeout_s":999999}]})", "timeout_s"},
        {"negative retries", R"({"tasks":[{"name":"a","argv":["x"],"retries":-1}]})", "retries"},
        {"too many retries", R"({"tasks":[{"name":"a","argv":["x"],"retries":11}]})", "retries"},
        {"fractional timeout", R"({"tasks":[{"name":"a","argv":["x"],"timeout_s":1.5}]})", "timeout_s"},
        {"bad env value", R"({"tasks":[{"name":"a","argv":["x"],"env":{"K":1}}]})", "env"},
        {"env key with '='", R"({"tasks":[{"name":"a","argv":["x"],"env":{"A=B":"1"}}]})", "env"},
        {"needs not strings", R"({"tasks":[{"name":"a","argv":["x"],"requires":[1]}]})", "requires"},
        {"repeat zero", R"({"tasks":[{"name":"a","argv":["x"],"repeat":0}]})", "repeat"},
        {"duplicate names", R"({"tasks":[{"name":"a","argv":["x"]},{"name":"a","argv":["y"]}]})", "duplicate task name 'a'"},
        {"repeat collides", R"({"tasks":[{"name":"a[1]","argv":["x"]},{"name":"a","argv":["y"],"repeat":2}]})", "duplicate"},
        {"bad defaults", R"({"defaults":{"timeout_s":"soon"},"tasks":[{"name":"a","argv":["x"]}]})", "defaults.timeout_s"},
    };
    for (const auto& c : cases) {
        const auto r = parse(c.json);
        EXPECT_NE(r.error.find(c.expect), std::string::npos) << c.what << ": got '" << r.error << "'";
        EXPECT_TRUE(r.job.tasks.empty()) << c.what;
    }
}

TEST(Jobs, ErrorNamesTheOffendingTask) {
    const auto r = parse(R"({"tasks":[{"name":"ok","argv":["x"]},{"name":"broken","argv":[]}]})");
    EXPECT_NE(r.error.find("tasks[1] ('broken')"), std::string::npos) << r.error;
}

TEST(Labels, ExactAndPresenceMatching) {
    const std::vector<std::string> agent = {"os=linux", "arch=x86_64", "kvm=true", "gpu"};
    EXPECT_TRUE(labels_satisfy(agent, {}));
    EXPECT_TRUE(labels_satisfy(agent, {"kvm=true"}));
    EXPECT_TRUE(labels_satisfy(agent, {"kvm"}));       // any value
    EXPECT_TRUE(labels_satisfy(agent, {"gpu"}));       // bare label
    EXPECT_TRUE(labels_satisfy(agent, {"os=linux", "kvm=true"}));
    EXPECT_FALSE(labels_satisfy(agent, {"kvm=false"}));
    EXPECT_FALSE(labels_satisfy(agent, {"os=linux", "tpm"}));  // all requirements must hold
    EXPECT_FALSE(labels_satisfy(agent, {"kv"}));               // no prefix matching
    EXPECT_FALSE(labels_satisfy({}, {"kvm"}));
}
