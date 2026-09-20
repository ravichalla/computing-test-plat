#include "vmtest/junit.hpp"

#include <iomanip>
#include <map>
#include <sstream>

#include "vmtest/util.hpp"

namespace vmtest {

namespace {

struct Counts {
    int tests = 0, failures = 0, skipped = 0;
    double seconds = 0;
};

Counts count(const std::vector<const TestResult*>& rs) {
    Counts c;
    for (const auto* r : rs) {
        ++c.tests;
        c.seconds += r->seconds;
        if (r->outcome == Outcome::Failed) ++c.failures;
        if (r->outcome == Outcome::Skipped) ++c.skipped;
    }
    return c;
}

}  // namespace

std::string render_junit(const std::string& run_name, const std::vector<TestResult>& results) {
    // Group by suite, preserving first-seen order.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const TestResult*>> by_suite;
    for (const auto& r : results) {
        if (!by_suite.count(r.suite)) order.push_back(r.suite);
        by_suite[r.suite].push_back(&r);
    }

    std::vector<const TestResult*> all;
    for (const auto& r : results) all.push_back(&r);
    const Counts total = count(all);

    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    os << "<testsuites name=\"" << xml_escape(run_name) << "\" tests=\"" << total.tests
       << "\" failures=\"" << total.failures << "\" skipped=\"" << total.skipped << "\" time=\""
       << total.seconds << "\">\n";

    for (const auto& suite : order) {
        const auto& rs = by_suite[suite];
        const Counts c = count(rs);
        os << "  <testsuite name=\"" << xml_escape(suite) << "\" tests=\"" << c.tests << "\" failures=\""
           << c.failures << "\" skipped=\"" << c.skipped << "\" time=\"" << c.seconds << "\">\n";
        for (const auto* r : rs) {
            os << "    <testcase classname=\"" << xml_escape(r->suite) << "\" name=\"" << xml_escape(r->name)
               << "\" time=\"" << r->seconds << "\"";
            switch (r->outcome) {
                case Outcome::Passed:
                    os << "/>\n";
                    break;
                case Outcome::Failed:
                    os << ">\n      <failure message=\"" << xml_escape(r->message) << "\"/>\n    </testcase>\n";
                    break;
                case Outcome::Skipped:
                    os << ">\n      <skipped message=\"" << xml_escape(r->message) << "\"/>\n    </testcase>\n";
                    break;
            }
        }
        os << "  </testsuite>\n";
    }
    os << "</testsuites>\n";
    return os.str();
}

}  // namespace vmtest
