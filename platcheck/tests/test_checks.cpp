#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "platcheck/checks.hpp"

namespace fs = std::filesystem;
using namespace platcheck;

namespace {

// A throwaway directory that mimics the parts of / that platcheck reads.
class FakeRoot : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("platcheck-test-" + std::to_string(::getpid()) + "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override { fs::remove_all(root_); }

    void write(const std::string& rel, const std::string& content) {
        fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }
    void mkdir(const std::string& rel) { fs::create_directories(root_ / rel); }
    void symlink(const std::string& rel, const std::string& target) {
        fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        fs::create_symlink(target, p);
    }

    // Adds a PCI device to a fake IOMMU group.
    void add_pci(int group, const std::string& bdf, const std::string& vendor,
                 const std::string& cls, const std::string& driver = "") {
        mkdir("sys/kernel/iommu_groups/" + std::to_string(group) + "/devices/" + bdf);
        write("sys/bus/pci/devices/" + bdf + "/vendor", vendor + "\n");
        write("sys/bus/pci/devices/" + bdf + "/device", "0x2684\n");
        write("sys/bus/pci/devices/" + bdf + "/class", cls + "\n");
        if (!driver.empty()) symlink("sys/bus/pci/devices/" + bdf + "/driver", "../../../../bus/pci/drivers/" + driver);
    }

    Checker checker() const { return Checker(root_.string()); }

    fs::path root_;
};

}  // namespace

// ---- pure parsers -----------------------------------------------------------

TEST(Parsers, CpuFlagMatchesWholeTokensOnly) {
    const std::string info = "processor : 0\nflags : fpu vme vmxfake sse4_2\n";
    EXPECT_FALSE(cpu_has_flag(info, "vmx"));
    EXPECT_TRUE(cpu_has_flag(info, "sse4_2"));
}

TEST(Parsers, CpuFlagDetectsSvm) {
    EXPECT_TRUE(cpu_has_flag("flags\t\t: fpu svm lm\n", "svm"));
}

TEST(Parsers, CpuinfoWithoutFlagsLine) {
    const std::string arm = "processor : 0\nFeatures : fp asimd evtstrm\n";
    EXPECT_FALSE(cpuinfo_has_flags_line(arm));
    EXPECT_TRUE(cpuinfo_has_flags_line("flags : fpu\n"));
}

TEST(Parsers, MeminfoValue) {
    const std::string m = "MemTotal:       16318412 kB\nHugePages_Total:      64\nHugepagesize:       2048 kB\n";
    EXPECT_EQ(meminfo_value(m, "HugePages_Total"), 64);
    EXPECT_EQ(meminfo_value(m, "Hugepagesize"), 2048);
    EXPECT_FALSE(meminfo_value(m, "Nope").has_value());
}

TEST(Parsers, CmdlineHelpers) {
    const std::string c = "BOOT_IMAGE=/vmlinuz root=/dev/sda1 intel_iommu=on iommu=pt quiet";
    EXPECT_TRUE(cmdline_has(c, "intel_iommu"));
    EXPECT_TRUE(cmdline_has(c, "quiet"));
    EXPECT_FALSE(cmdline_has(c, "amd_iommu"));
    EXPECT_EQ(cmdline_value(c, "iommu"), "pt");
    EXPECT_FALSE(cmdline_value(c, "missing").has_value());
}

TEST(Output, JsonEscape) {
    EXPECT_EQ(json_escape("a\"b\\c\n"), "a\\\"b\\\\c\\n");
    EXPECT_EQ(json_escape(std::string(1, '\x01')), "\\u0001");
}

// ---- individual checks ----------------------------------------------------

TEST_F(FakeRoot, CpuVirtPassesOnIntel) {
    write("proc/cpuinfo", "flags : fpu vmx sse\n");
    EXPECT_EQ(checker().check_cpu_virt().status, Status::Pass);
}

TEST_F(FakeRoot, CpuVirtFailsWithoutExtensions) {
    write("proc/cpuinfo", "flags : fpu sse\n");
    auto r = checker().check_cpu_virt();
    EXPECT_EQ(r.status, Status::Fail);
    EXPECT_FALSE(r.remediation.empty());
}

TEST_F(FakeRoot, CpuVirtSkipsOnNonX86) {
    write("proc/cpuinfo", "Features : fp asimd\n");
    EXPECT_EQ(checker().check_cpu_virt().status, Status::Skip);
}

TEST_F(FakeRoot, KvmDeviceMissingFails) {
    EXPECT_EQ(checker().check_kvm_device().status, Status::Fail);
}

TEST_F(FakeRoot, KvmDevicePresentPasses) {
    write("dev/kvm", "");
    EXPECT_EQ(checker().check_kvm_device().status, Status::Pass);
}

TEST_F(FakeRoot, KvmModuleDetection) {
    EXPECT_EQ(checker().check_kvm_module().status, Status::Fail);
    mkdir("sys/module/kvm_amd");
    EXPECT_EQ(checker().check_kvm_module().status, Status::Pass);
}

TEST_F(FakeRoot, NestedVirt) {
    EXPECT_EQ(checker().check_nested_virt().status, Status::Skip);
    write("sys/module/kvm_intel/parameters/nested", "N\n");
    EXPECT_EQ(checker().check_nested_virt().status, Status::Warn);
    write("sys/module/kvm_intel/parameters/nested", "Y\n");
    EXPECT_EQ(checker().check_nested_virt().status, Status::Pass);
}

TEST_F(FakeRoot, IommuFailsWhenNoGroups) {
    write("proc/cmdline", "quiet\n");
    auto r = checker().check_iommu();
    EXPECT_EQ(r.status, Status::Fail);
    EXPECT_NE(r.detail.find("intel_iommu"), std::string::npos);
}

TEST_F(FakeRoot, IommuPassesWithGroups) {
    write("proc/cmdline", "intel_iommu=on iommu=pt\n");
    add_pci(1, "0000:00:01.0", "0x8086", "0x060400");
    auto r = checker().check_iommu();
    EXPECT_EQ(r.status, Status::Pass);
    EXPECT_NE(r.detail.find("iommu=pt"), std::string::npos);
}

TEST_F(FakeRoot, VfioModules) {
    EXPECT_EQ(checker().check_vfio_modules().status, Status::Warn);
    mkdir("sys/module/vfio");
    mkdir("sys/module/vfio_pci");
    mkdir("sys/module/vfio_iommu_type1");
    EXPECT_EQ(checker().check_vfio_modules().status, Status::Pass);
}

TEST_F(FakeRoot, Hugepages) {
    write("proc/meminfo", "HugePages_Total:       0\nHugepagesize:       2048 kB\n");
    EXPECT_EQ(checker().check_hugepages().status, Status::Warn);
    write("proc/meminfo", "HugePages_Total:      16\nHugepagesize:       2048 kB\n");
    EXPECT_EQ(checker().check_hugepages().status, Status::Pass);
}

TEST_F(FakeRoot, ConfidentialComputingDetectsSevAndTdx) {
    EXPECT_EQ(checker().check_confidential_computing().status, Status::Skip);
    write("sys/module/kvm_amd/parameters/sev", "Y\n");
    write("sys/module/kvm_amd/parameters/sev_snp", "0\n");
    auto r = checker().check_confidential_computing();
    EXPECT_EQ(r.status, Status::Pass);
    EXPECT_NE(r.detail.find("kvm_amd.sev"), std::string::npos);
    EXPECT_EQ(r.detail.find("sev_snp"), std::string::npos);  // disabled param not reported
}

// ---- IOMMU / GPU isolation --------------------------------------------------

TEST_F(FakeRoot, IommuGroupsAreParsedAndSorted) {
    add_pci(10, "0000:02:00.0", "0x10DE", "0x030200", "vfio-pci");
    add_pci(2, "0000:00:1f.0", "0x8086", "0x060100");
    auto groups = read_iommu_groups(root_.string());
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].id, 2);
    EXPECT_EQ(groups[1].id, 10);
    ASSERT_EQ(groups[1].devices.size(), 1u);
    EXPECT_EQ(groups[1].devices[0].vendor, "0x10de");  // lower-cased
    EXPECT_EQ(groups[1].devices[0].driver, "vfio-pci");
}

TEST_F(FakeRoot, GpuIsolationSkipsWithoutNvidia) {
    add_pci(1, "0000:00:1f.0", "0x8086", "0x060100");
    EXPECT_EQ(checker().check_gpu_isolation().status, Status::Skip);
}

TEST_F(FakeRoot, GpuIsolationPassesWhenGroupOnlyHasGpuAndAudio) {
    add_pci(1, "0000:01:00.0", "0x10de", "0x030000", "vfio-pci");
    add_pci(1, "0000:01:00.1", "0x10de", "0x040300", "vfio-pci");
    add_pci(1, "0000:00:01.0", "0x8086", "0x060400");  // bridge is tolerated
    auto r = checker().check_gpu_isolation();
    EXPECT_EQ(r.status, Status::Pass);
    EXPECT_NE(r.detail.find("vfio-pci"), std::string::npos);
}

TEST_F(FakeRoot, GpuIsolationWarnsWhenGroupSharedWithOtherEndpoint) {
    add_pci(4, "0000:01:00.0", "0x10de", "0x030200", "nvidia");
    add_pci(4, "0000:02:00.0", "0x144d", "0x010802");  // NVMe in the same group
    auto r = checker().check_gpu_isolation();
    EXPECT_EQ(r.status, Status::Warn);
    EXPECT_NE(r.detail.find("0000:02:00.0"), std::string::npos);
    EXPECT_FALSE(r.remediation.empty());
}

// ---- end to end -------------------------------------------------------------

TEST_F(FakeRoot, RunAllAndExitCodePolicy) {
    write("proc/cpuinfo", "flags : vmx\n");
    write("proc/sys/kernel/osrelease", "6.8.0-test\n");
    write("dev/kvm", "");
    mkdir("sys/module/kvm_intel");
    add_pci(1, "0000:00:01.0", "0x8086", "0x060400");
    auto results = checker().run_all();
    EXPECT_EQ(results.size(), 10u);
    EXPECT_EQ(exit_code_for(results), 0);  // warnings must not fail the run

    // Removing /dev/kvm turns a check into a hard failure.
    fs::remove(root_ / "dev/kvm");
    EXPECT_EQ(exit_code_for(checker().run_all()), 1);
}

TEST_F(FakeRoot, JsonReportShape) {
    write("proc/cpuinfo", "flags : vmx\n");
    auto json = render_json(checker().run_all());
    EXPECT_NE(json.find("\"id\": \"cpu_virt\""), std::string::npos);
    EXPECT_NE(json.find("\"status\": \"PASS\""), std::string::npos);
    EXPECT_NE(json.find("\"ok\":"), std::string::npos);
}
