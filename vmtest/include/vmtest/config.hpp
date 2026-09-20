#pragma once

#include <string>

namespace vmtest {

struct Config {
    std::string uri = "qemu:///system";
    std::string image;                 // base disk image (must contain qemu-guest-agent)
    std::string image_format = "qcow2";
    std::string work_dir = "/var/tmp"; // where throwaway overlay disks are created
    unsigned vcpus = 2;
    unsigned mem_mib = 1024;
    int boot_timeout_s = 180;
    int shutdown_timeout_s = 90;
    int iterations = 0;                // >0 enables the boot/destroy soak test
    std::string filter;                // substring filter on "suite.name"
    std::string junit_path;            // write JUnit XML here if non-empty
    bool list_only = false;
};

struct ParseResult {
    Config cfg;
    std::string error;  // non-empty on a usage error
    bool help = false;
};

ParseResult parse_args(int argc, const char* const* argv);
std::string usage_text(const std::string& prog);

}  // namespace vmtest
