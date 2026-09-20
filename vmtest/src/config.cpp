#include "vmtest/config.hpp"

#include <cstdlib>
#include <sstream>

namespace vmtest {

namespace {

bool parse_uint(const std::string& s, long lo, long hi, long* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

}  // namespace

std::string usage_text(const std::string& prog) {
    std::ostringstream os;
    os << "Usage: " << prog << " --image PATH [options]\n\n"
       << "Boots a KVM guest through libvirt and validates lifecycle and guest behaviour.\n"
       << "The image must have qemu-guest-agent installed and enabled (see scripts/prepare_image.sh).\n\n"
       << "Options:\n"
       << "  --image PATH           base disk image (required unless --list)\n"
       << "  --image-format FMT     format of the base image (default: qcow2)\n"
       << "  --uri URI              libvirt URI (default: qemu:///system)\n"
       << "  --work-dir DIR         directory for throwaway overlay disks (default: /var/tmp)\n"
       << "  --vcpus N              guest vCPUs (default: 2)\n"
       << "  --memory-mib N         guest memory in MiB (default: 1024)\n"
       << "  --boot-timeout S       seconds to wait for the guest agent (default: 180)\n"
       << "  --shutdown-timeout S   seconds to wait for graceful shutdown (default: 90)\n"
       << "  --iterations N         run the boot/destroy soak test N times (default: off)\n"
       << "  --filter TEXT          only run tests whose 'suite.name' contains TEXT\n"
       << "  --junit FILE           also write JUnit XML to FILE\n"
       << "  --list                 list test names and exit\n"
       << "  -h, --help             show this help\n\n"
       << "Exit status: 0 = all passed (skips allowed), 1 = a test failed, 2 = usage error.\n";
    return os.str();
}

ParseResult parse_args(int argc, const char* const* argv) {
    ParseResult r;
    auto fail = [&](const std::string& msg) {
        r.error = msg;
        return r;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string v;
        long n = 0;

        if (a == "-h" || a == "--help") {
            r.help = true;
        } else if (a == "--list") {
            r.cfg.list_only = true;
        } else if (a == "--image") {
            if (!value(&r.cfg.image)) return fail("--image needs a value");
        } else if (a == "--image-format") {
            if (!value(&r.cfg.image_format)) return fail("--image-format needs a value");
        } else if (a == "--uri") {
            if (!value(&r.cfg.uri)) return fail("--uri needs a value");
        } else if (a == "--work-dir") {
            if (!value(&r.cfg.work_dir)) return fail("--work-dir needs a value");
        } else if (a == "--filter") {
            if (!value(&r.cfg.filter)) return fail("--filter needs a value");
        } else if (a == "--junit") {
            if (!value(&r.cfg.junit_path)) return fail("--junit needs a value");
        } else if (a == "--vcpus") {
            if (!value(&v) || !parse_uint(v, 1, 512, &n)) return fail("--vcpus needs an integer in 1..512");
            r.cfg.vcpus = static_cast<unsigned>(n);
        } else if (a == "--memory-mib") {
            if (!value(&v) || !parse_uint(v, 128, 1 << 20, &n)) return fail("--memory-mib needs an integer >= 128");
            r.cfg.mem_mib = static_cast<unsigned>(n);
        } else if (a == "--boot-timeout") {
            if (!value(&v) || !parse_uint(v, 1, 86400, &n)) return fail("--boot-timeout needs a positive integer");
            r.cfg.boot_timeout_s = static_cast<int>(n);
        } else if (a == "--shutdown-timeout") {
            if (!value(&v) || !parse_uint(v, 1, 86400, &n)) return fail("--shutdown-timeout needs a positive integer");
            r.cfg.shutdown_timeout_s = static_cast<int>(n);
        } else if (a == "--iterations") {
            if (!value(&v) || !parse_uint(v, 0, 100000, &n)) return fail("--iterations needs a non-negative integer");
            r.cfg.iterations = static_cast<int>(n);
        } else {
            return fail("unknown option: " + a);
        }
    }
    return r;
}

}  // namespace vmtest
