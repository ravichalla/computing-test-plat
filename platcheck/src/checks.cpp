#include "platcheck/checks.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace platcheck {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

std::string to_string(Status s) {
    switch (s) {
        case Status::Pass: return "PASS";
        case Status::Warn: return "WARN";
        case Status::Fail: return "FAIL";
        case Status::Skip: return "SKIP";
    }
    return "?";
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

bool cpuinfo_has_flags_line(const std::string& cpuinfo) {
    std::istringstream in(cpuinfo);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("flags", 0) == 0 && line.find(':') != std::string::npos) return true;
    }
    return false;
}

bool cpu_has_flag(const std::string& cpuinfo, const std::string& flag) {
    std::istringstream in(cpuinfo);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("flags", 0) != 0) continue;
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        for (const auto& tok : split_ws(line.substr(pos + 1))) {
            if (tok == flag) return true;
        }
        return false;  // only the first CPU's flags are inspected
    }
    return false;
}

std::optional<long> meminfo_value(const std::string& meminfo, const std::string& key) {
    std::istringstream in(meminfo);
    std::string line;
    const std::string prefix = key + ":";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0) continue;
        std::istringstream vs(line.substr(prefix.size()));
        long v = 0;
        if (vs >> v) return v;
        return std::nullopt;
    }
    return std::nullopt;
}

bool cmdline_has(const std::string& cmdline, const std::string& key) {
    for (const auto& tok : split_ws(cmdline)) {
        if (tok == key || tok.rfind(key + "=", 0) == 0) return true;
    }
    return false;
}

std::optional<std::string> cmdline_value(const std::string& cmdline, const std::string& key) {
    const std::string prefix = key + "=";
    for (const auto& tok : split_ws(cmdline)) {
        if (tok.rfind(prefix, 0) == 0) return tok.substr(prefix.size());
    }
    return std::nullopt;
}

static std::string read_trimmed(const std::string& path) {
    auto c = read_file(path);
    return c ? trim(*c) : std::string();
}

std::vector<IommuGroup> read_iommu_groups(const std::string& root) {
    std::vector<IommuGroup> groups;
    std::error_code ec;
    const fs::path base = fs::path(root + "/sys/kernel/iommu_groups");
    if (!fs::is_directory(base, ec)) return groups;

    for (const auto& entry : fs::directory_iterator(base, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }

        IommuGroup g;
        g.id = std::stoi(name);
        std::error_code ec2;
        for (const auto& dev : fs::directory_iterator(entry.path() / "devices", ec2)) {
            IommuDevice d;
            d.bdf = dev.path().filename().string();
            const std::string pci = root + "/sys/bus/pci/devices/" + d.bdf;
            d.vendor = to_lower(read_trimmed(pci + "/vendor"));
            d.device = to_lower(read_trimmed(pci + "/device"));
            d.cls = to_lower(read_trimmed(pci + "/class"));
            std::error_code ec3;
            fs::path drv = pci + "/driver";
            if (fs::is_symlink(drv, ec3)) {
                d.driver = fs::read_symlink(drv, ec3).filename().string();
            }
            g.devices.push_back(std::move(d));
        }
        std::sort(g.devices.begin(), g.devices.end(),
                  [](const IommuDevice& a, const IommuDevice& b) { return a.bdf < b.bdf; });
        groups.push_back(std::move(g));
    }
    std::sort(groups.begin(), groups.end(),
              [](const IommuGroup& a, const IommuGroup& b) { return a.id < b.id; });
    return groups;
}

static CheckResult make(const char* id, const char* title, Status s, std::string detail,
                        std::string remediation = "") {
    CheckResult r;
    r.id = id;
    r.title = title;
    r.status = s;
    r.detail = std::move(detail);
    r.remediation = std::move(remediation);
    return r;
}

static bool param_enabled(const std::string& v) { return v == "Y" || v == "y" || v == "1"; }

// ---------------------------------------------------------------------------
// Checker
// ---------------------------------------------------------------------------

Checker::Checker(std::string root) : root_(std::move(root)) {
    while (!root_.empty() && root_.back() == '/') root_.pop_back();
}

std::vector<CheckResult> Checker::run_all() const {
    return {check_kernel(),       check_cpu_virt(),   check_kvm_device(),
            check_kvm_module(),   check_nested_virt(), check_iommu(),
            check_vfio_modules(), check_hugepages(),  check_gpu_isolation(),
            check_confidential_computing()};
}

CheckResult Checker::check_kernel() const {
    auto rel = read_file(path("/proc/sys/kernel/osrelease"));
    if (!rel) return make("kernel", "Kernel version", Status::Skip, "osrelease not readable");
    return make("kernel", "Kernel version", Status::Pass, trim(*rel));
}

CheckResult Checker::check_cpu_virt() const {
    const char* id = "cpu_virt";
    const char* title = "CPU virtualization extensions";
    auto info = read_file(path("/proc/cpuinfo"));
    if (!info) return make(id, title, Status::Skip, "/proc/cpuinfo not readable");
    if (!cpuinfo_has_flags_line(*info)) {
        return make(id, title, Status::Skip,
                    "no x86 'flags' line (non-x86 host?); rely on the /dev/kvm check");
    }
    if (cpu_has_flag(*info, "vmx")) return make(id, title, Status::Pass, "Intel VT-x (vmx) present");
    if (cpu_has_flag(*info, "svm")) return make(id, title, Status::Pass, "AMD-V (svm) present");
    return make(id, title, Status::Fail, "neither vmx nor svm present in cpu flags",
                "Enable VT-x/AMD-V in firmware; if this is a VM, enable nested virtualization "
                "on the parent hypervisor.");
}

CheckResult Checker::check_kvm_device() const {
    const char* id = "kvm_device";
    const char* title = "/dev/kvm accessible";
    const std::string dev = path("/dev/kvm");
    std::error_code ec;
    if (!fs::exists(dev, ec)) {
        return make(id, title, Status::Fail, "/dev/kvm does not exist",
                    "Load the kvm module (modprobe kvm_intel / kvm_amd) and check firmware settings.");
    }
    if (access(dev.c_str(), R_OK | W_OK) != 0) {
        return make(id, title, Status::Warn, "/dev/kvm exists but is not read/write for this user",
                    "Add your user to the 'kvm' group, or run as a user that can open /dev/kvm.");
    }
    return make(id, title, Status::Pass, "/dev/kvm is present and read/write");
}

CheckResult Checker::check_kvm_module() const {
    const char* id = "kvm_module";
    const char* title = "KVM kernel module";
    std::error_code ec;
    if (fs::exists(path("/sys/module/kvm_intel"), ec)) return make(id, title, Status::Pass, "kvm_intel loaded");
    if (fs::exists(path("/sys/module/kvm_amd"), ec)) return make(id, title, Status::Pass, "kvm_amd loaded");
    if (fs::exists(path("/sys/module/kvm"), ec)) {
        return make(id, title, Status::Pass, "kvm loaded (no vendor module; e.g. arm64)");
    }
    return make(id, title, Status::Fail, "no kvm module loaded",
                "modprobe kvm_intel or kvm_amd (check dmesg for why it failed to load).");
}

CheckResult Checker::check_nested_virt() const {
    const char* id = "nested_virt";
    const char* title = "Nested virtualization";
    for (const char* mod : {"kvm_intel", "kvm_amd"}) {
        auto v = read_file(path(std::string("/sys/module/") + mod + "/parameters/nested"));
        if (!v) continue;
        const std::string val = trim(*v);
        if (param_enabled(val)) {
            return make(id, title, Status::Pass, std::string(mod) + ".nested = " + val);
        }
        return make(id, title, Status::Warn, std::string(mod) + ".nested = " + val,
                    std::string("Reload with 'options ") + mod +
                        " nested=1' in /etc/modprobe.d/ if you need VMs inside VMs.");
    }
    return make(id, title, Status::Skip, "no nested parameter exposed by the loaded kvm module");
}

CheckResult Checker::check_iommu() const {
    const char* id = "iommu";
    const char* title = "IOMMU enabled";
    auto groups = read_iommu_groups(root_);
    const std::string cmdline = read_trimmed(path("/proc/cmdline"));
    if (!groups.empty()) {
        std::string d = std::to_string(groups.size()) + " IOMMU groups";
        if (auto pt = cmdline_value(cmdline, "iommu"); pt) d += ", iommu=" + *pt;
        return make(id, title, Status::Pass, d);
    }
    std::string d = "no IOMMU groups found";
    if (!cmdline_has(cmdline, "intel_iommu") && !cmdline_has(cmdline, "amd_iommu")) {
        d += "; kernel cmdline has no intel_iommu/amd_iommu option";
    }
    return make(id, title, Status::Fail, d,
                "Enable VT-d/AMD-Vi in firmware and boot with intel_iommu=on (or amd_iommu=on) "
                "and optionally iommu=pt.");
}

CheckResult Checker::check_vfio_modules() const {
    const char* id = "vfio";
    const char* title = "VFIO modules";
    std::error_code ec;
    const bool vfio = fs::exists(path("/sys/module/vfio"), ec);
    const bool pci = fs::exists(path("/sys/module/vfio_pci"), ec);
    const bool type1 = fs::exists(path("/sys/module/vfio_iommu_type1"), ec);
    std::string d = std::string("vfio=") + (vfio ? "y" : "n") + " vfio_pci=" + (pci ? "y" : "n") +
                    " vfio_iommu_type1=" + (type1 ? "y" : "n");
    if (vfio && pci && type1) return make(id, title, Status::Pass, d);
    return make(id, title, Status::Warn, d,
                "modprobe vfio-pci (only needed for PCI device passthrough).");
}

CheckResult Checker::check_hugepages() const {
    const char* id = "hugepages";
    const char* title = "Hugepages reserved";
    auto info = read_file(path("/proc/meminfo"));
    if (!info) return make(id, title, Status::Skip, "/proc/meminfo not readable");
    auto total = meminfo_value(*info, "HugePages_Total");
    auto size = meminfo_value(*info, "Hugepagesize");
    if (!total) return make(id, title, Status::Skip, "HugePages_Total not reported");
    std::string d = "HugePages_Total=" + std::to_string(*total);
    if (size) d += ", Hugepagesize=" + std::to_string(*size) + " kB";
    if (*total > 0) return make(id, title, Status::Pass, d);
    return make(id, title, Status::Warn, d,
                "Reserve hugepages (vm.nr_hugepages) for lower-jitter, higher-throughput guests.");
}

// PCI class helpers. Class strings look like "0x030000".
static bool is_display_class(const std::string& c) { return c.rfind("0x03", 0) == 0; }
static bool is_bridge_class(const std::string& c) { return c.rfind("0x06", 0) == 0; }
static bool is_audio_class(const std::string& c) { return c.rfind("0x0403", 0) == 0; }
static bool is_nvidia(const IommuDevice& d) { return d.vendor == "0x10de"; }

CheckResult Checker::check_gpu_isolation() const {
    const char* id = "gpu_isolation";
    const char* title = "NVIDIA GPU IOMMU isolation";
    auto groups = read_iommu_groups(root_);

    Status worst = Status::Skip;
    std::string detail;
    int gpus = 0;

    auto rank = [](Status x) {
        return x == Status::Fail ? 3 : x == Status::Warn ? 2 : x == Status::Pass ? 1 : 0;
    };
    auto bump = [&](Status s) {
        if (rank(s) > rank(worst)) worst = s;
    };

    for (const auto& g : groups) {
        for (const auto& dev : g.devices) {
            if (!is_nvidia(dev) || !is_display_class(dev.cls)) continue;
            ++gpus;
            std::vector<std::string> others;
            for (const auto& o : g.devices) {
                if (o.bdf == dev.bdf) continue;
                if (is_bridge_class(o.cls)) continue;                 // bridges are fine
                if (is_nvidia(o) && is_audio_class(o.cls)) continue;  // GPU's own HDA function
                others.push_back(o.bdf);
            }
            if (!detail.empty()) detail += "; ";
            detail += dev.bdf + " (group " + std::to_string(g.id) + ", driver " +
                      (dev.driver.empty() ? "none" : dev.driver) + ")";
            if (others.empty()) {
                bump(Status::Pass);
            } else {
                bump(Status::Warn);
                detail += " shares its group with:";
                for (const auto& b : others) detail += " " + b;
            }
        }
    }
    if (gpus == 0) {
        return make(id, title, Status::Skip, "no NVIDIA display device found in any IOMMU group");
    }
    std::string rem;
    if (worst == Status::Warn) {
        rem = "Endpoints sharing an IOMMU group must be passed through together. Try another slot "
              "or a platform with ACS support.";
    }
    return make(id, title, worst, detail, rem);
}

CheckResult Checker::check_confidential_computing() const {
    const char* id = "confidential_computing";
    const char* title = "Confidential computing support (SEV/TDX)";
    std::string found;
    auto probe = [&](const char* mod, const char* param) {
        auto v = read_file(path(std::string("/sys/module/") + mod + "/parameters/" + param));
        if (v && param_enabled(trim(*v))) {
            if (!found.empty()) found += ", ";
            found += std::string(mod) + "." + param;
        }
    };
    probe("kvm_amd", "sev");
    probe("kvm_amd", "sev_es");
    probe("kvm_amd", "sev_snp");
    probe("kvm_intel", "tdx");

    std::error_code ec;
    if (fs::exists(path("/dev/sev"), ec)) {
        if (!found.empty()) found += ", ";
        found += "/dev/sev";
    }
    if (!found.empty()) return make(id, title, Status::Pass, "detected: " + found);
    return make(id, title, Status::Skip,
                "no SEV/SEV-ES/SEV-SNP/TDX indicators exposed by this kernel or CPU");
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string render_text(const std::vector<CheckResult>& results) {
    std::ostringstream os;
    int pass = 0, warn = 0, fail = 0, skip = 0;
    for (const auto& r : results) {
        os << "[" << to_string(r.status) << "] " << r.title << ": " << r.detail << "\n";
        if (!r.remediation.empty() && (r.status == Status::Warn || r.status == Status::Fail)) {
            os << "       -> " << r.remediation << "\n";
        }
        switch (r.status) {
            case Status::Pass: ++pass; break;
            case Status::Warn: ++warn; break;
            case Status::Fail: ++fail; break;
            case Status::Skip: ++skip; break;
        }
    }
    os << "\nSummary: " << pass << " passed, " << warn << " warnings, " << fail << " failed, "
       << skip << " skipped\n";
    return os.str();
}

std::string render_json(const std::vector<CheckResult>& results) {
    std::ostringstream os;
    os << "{\n  \"checks\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        os << "    {\"id\": \"" << json_escape(r.id) << "\", \"title\": \"" << json_escape(r.title)
           << "\", \"status\": \"" << to_string(r.status) << "\", \"detail\": \""
           << json_escape(r.detail) << "\", \"remediation\": \"" << json_escape(r.remediation)
           << "\"}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    os << "  ],\n  \"ok\": " << (exit_code_for(results) == 0 ? "true" : "false") << "\n}\n";
    return os.str();
}

std::string render_groups(const std::vector<IommuGroup>& groups) {
    std::ostringstream os;
    if (groups.empty()) return "No IOMMU groups found.\n";
    for (const auto& g : groups) {
        os << "IOMMU group " << g.id << "\n";
        for (const auto& d : g.devices) {
            os << "  " << d.bdf << "  vendor=" << d.vendor << " device=" << d.device
               << " class=" << d.cls << " driver=" << (d.driver.empty() ? "-" : d.driver) << "\n";
        }
    }
    return os.str();
}

int exit_code_for(const std::vector<CheckResult>& results) {
    for (const auto& r : results) {
        if (r.status == Status::Fail) return 1;
    }
    return 0;
}

}  // namespace platcheck
