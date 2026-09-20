#pragma once

#include <string>
#include <vector>

namespace vmtest {

enum class Outcome { Passed, Failed, Skipped };

struct TestResult {
    std::string suite;  // becomes the JUnit "classname" and testsuite name
    std::string name;
    Outcome outcome = Outcome::Passed;
    std::string message;  // failure / skip reason
    double seconds = 0.0;
};

// Renders JUnit-style XML that CI systems (GitHub Actions, Jenkins, ...) understand.
std::string render_junit(const std::string& run_name, const std::vector<TestResult>& results);

}  // namespace vmtest
