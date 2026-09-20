// platcheck - host readiness checks for KVM virtualization and GPU passthrough.
//
// All filesystem access goes through a configurable "root" prefix so the
// checks can be unit-tested against a fake /proc + /sys tree.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace platcheck {

enum class Status { Pass, Warn, Fail, Skip };

std::string to_string(Status s);

struct CheckResult {
    std::string id;           // stable machine-readable id, e.g. "kvm_device"
    std::string title;        // human-readable title
    Status status = Status::Skip;
    std::string detail;       // what was observed
    std::string remediation;  // what to do about a Warn/Fail (may be empty)
};

struct IommuDevice {
    std::string bdf;     // e.g. "0000:01:00.0"
    std::string vendor;  // e.g. "0x10de"
    std::string device;  // e.g. "0x2684"
    std::string cls;     // e.g. "0x030000"
    std::string driver;  // bound driver name, empty if none
};

struct IommuGroup {
    int id = 0;
    std::vector<IommuDevice> devices;
};

// ---- small parsing helpers (exposed for unit tests) -----------------------

std::optional<std::string> read_file(const std::string& path);
std::string trim(const std::string& s);
std::string to_lower(std::string s);

// True if the first "flags" line of /proc/cpuinfo contains the exact token.
bool cpu_has_flag(const std::string& cpuinfo, const std::string& flag);
// True if /proc/cpuinfo has an x86-style "flags" line at all.
bool cpuinfo_has_flags_line(const std::string& cpuinfo);
// Value (in the units meminfo reports, usually kB or a bare count) for a key.
std::optional<long> meminfo_value(const std::string& meminfo, const std::string& key);
// Kernel command line helpers.
bool cmdline_has(const std::string& cmdline, const std::string& key);
std::optional<std::string> cmdline_value(const std::string& cmdline, const std::string& key);

// Enumerate IOMMU groups under <root>/sys/kernel/iommu_groups.
std::vector<IommuGroup> read_iommu_groups(const std::string& root);

// ---- the checker -----------------------------------------------------------

class Checker {
public:
    explicit Checker(std::string root = "");

    std::vector<CheckResult> run_all() const;

    CheckResult check_kernel() const;
    CheckResult check_cpu_virt() const;
    CheckResult check_kvm_device() const;
    CheckResult check_kvm_module() const;
    CheckResult check_nested_virt() const;
    CheckResult check_iommu() const;
    CheckResult check_vfio_modules() const;
    CheckResult check_hugepages() const;
    CheckResult check_gpu_isolation() const;
    CheckResult check_confidential_computing() const;

private:
    std::string path(const std::string& p) const { return root_ + p; }
    std::string root_;
};

// ---- output ---------------------------------------------------------------

std::string json_escape(const std::string& s);
std::string render_text(const std::vector<CheckResult>& results);
std::string render_json(const std::vector<CheckResult>& results);
std::string render_groups(const std::vector<IommuGroup>& groups);
// Exit-code policy: 1 if any check failed, otherwise 0. Warnings do not fail.
int exit_code_for(const std::vector<CheckResult>& results);

}  // namespace platcheck
