#include <filesystem>
#include <fstream>
#include <iostream>

#include "vmtest/config.hpp"
#include "vmtest/junit.hpp"
#include "vmtest/tests.hpp"

int main(int argc, char** argv) {
    using namespace vmtest;

    const ParseResult parsed = parse_args(argc, argv);
    if (parsed.help) {
        std::cout << usage_text(argv[0]);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "error: " << parsed.error << "\n\n" << usage_text(argv[0]);
        return 2;
    }
    const Config& cfg = parsed.cfg;
    const auto tests = build_tests();

    if (cfg.list_only) {
        for (const auto& t : tests) std::cout << t.suite << "." << t.name << "\n";
        return 0;
    }
    if (cfg.image.empty()) {
        std::cerr << "error: --image is required\n\n" << usage_text(argv[0]);
        return 2;
    }
    if (!std::filesystem::exists(cfg.image)) {
        std::cerr << "error: image not found: " << cfg.image << "\n";
        return 2;
    }

    Context ctx(cfg);
    const auto results = run_tests(ctx, tests, std::cout);

    int failed = 0, skipped = 0;
    for (const auto& r : results) {
        if (r.outcome == Outcome::Failed) ++failed;
        if (r.outcome == Outcome::Skipped) ++skipped;
    }
    std::cout << "\n" << results.size() << " tests, " << failed << " failed, " << skipped << " skipped\n";

    if (!cfg.junit_path.empty()) {
        std::ofstream out(cfg.junit_path);
        if (!out) {
            std::cerr << "error: cannot write " << cfg.junit_path << "\n";
            return 2;
        }
        out << render_junit("vmtest", results);
        std::cout << "JUnit report written to " << cfg.junit_path << "\n";
    }
    return failed == 0 ? 0 : 1;
}
