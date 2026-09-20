#include <cstring>
#include <iostream>

#include "platcheck/checks.hpp"

static void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--json] [--groups] [--root DIR]\n"
              << "  --json       machine-readable output\n"
              << "  --groups     also list IOMMU groups and their devices\n"
              << "  --root DIR   read /proc and /sys from DIR (for testing against a fake tree)\n"
              << "Exit status: 0 = no failed checks, 1 = at least one failed check, 2 = usage error\n";
}

int main(int argc, char** argv) {
    bool json = false, groups = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--json")) {
            json = true;
        } else if (!std::strcmp(argv[i], "--groups")) {
            groups = true;
        } else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) {
            root = argv[++i];
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    platcheck::Checker checker(root);
    const auto results = checker.run_all();

    std::cout << (json ? platcheck::render_json(results) : platcheck::render_text(results));
    if (groups && !json) {
        std::cout << "\n" << platcheck::render_groups(platcheck::read_iommu_groups(root));
    }
    return platcheck::exit_code_for(results);
}
