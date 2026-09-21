#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "dtest/policy.hpp"

namespace fs = std::filesystem;
using namespace dtest;

namespace {

class PolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("dtest-policy-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "allowed");
        fs::create_directories(root_ / "allowed-evil");
        std::ofstream(root_ / "allowed" / "tool") << "#!/bin/sh\n";
        std::ofstream(root_ / "allowed-evil" / "tool") << "#!/bin/sh\n";
        fs::create_symlink("/bin/sh", root_ / "allowed" / "escape");
    }
    void TearDown() override { fs::remove_all(root_); }
    std::string p(const std::string& rel) const { return (root_ / rel).string(); }
    fs::path root_;
};

}  // namespace

TEST_F(PolicyTest, AllowAnyPermitsEverything) {
    Policy pol;
    pol.allow_any = true;
    EXPECT_EQ(check_command(pol, "relative/path"), "");
    EXPECT_EQ(check_command(pol, "/anything"), "");
}

TEST_F(PolicyTest, EmptyAllowListDeniesEverything) {
    EXPECT_NE(check_command(Policy{}, "/bin/true"), "");
}

TEST_F(PolicyTest, RelativePathsAreRejected) {
    Policy pol;
    pol.allow = {p("allowed/")};
    EXPECT_NE(check_command(pol, "tool").find("absolute"), std::string::npos);
}

TEST_F(PolicyTest, ExactProgramMatch) {
    Policy pol;
    pol.allow = {p("allowed/tool")};
    EXPECT_EQ(check_command(pol, p("allowed/tool")), "");
    EXPECT_NE(check_command(pol, p("allowed-evil/tool")), "");
}

TEST_F(PolicyTest, DirectoryEntryAllowsItsContents) {
    Policy pol;
    pol.allow = {p("allowed/")};
    EXPECT_EQ(check_command(pol, p("allowed/tool")), "");
}

TEST_F(PolicyTest, SiblingDirectoryWithSharedPrefixIsNotAllowed) {
    Policy pol;
    pol.allow = {p("allowed/")};
    EXPECT_NE(check_command(pol, p("allowed-evil/tool")), "");
}

TEST_F(PolicyTest, DotDotCannotEscapeTheAllowedDirectory) {
    Policy pol;
    pol.allow = {p("allowed/")};
    EXPECT_NE(check_command(pol, p("allowed/../../../../bin/sh")), "");
    EXPECT_NE(check_command(pol, p("allowed/../allowed-evil/tool")), "");
}

TEST_F(PolicyTest, SymlinkInsideAllowedDirectoryCannotEscape) {
    Policy pol;
    pol.allow = {p("allowed/")};
    // allowed/escape -> /bin/sh, which lives outside the allowed directory.
    EXPECT_NE(check_command(pol, p("allowed/escape")), "");
}

TEST_F(PolicyTest, DirectoryItselfIsNotAProgram) {
    Policy pol;
    pol.allow = {p("allowed/")};
    EXPECT_NE(check_command(pol, p("allowed")), "");
    EXPECT_NE(check_command(pol, p("allowed/")), "");
}
