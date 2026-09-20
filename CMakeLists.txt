cmake_minimum_required(VERSION 3.16)
project(kvm_validation_suite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTING_SUITE "Build unit tests (needs GoogleTest)" ON)
option(BUILD_VMTEST "Build the libvirt-based VM test harness (needs libvirt + nlohmann-json)" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

enable_testing()

# ---- platcheck: host readiness checker (library + CLI + unit tests) ----------
add_library(platcheck_lib platcheck/src/checks.cpp)
target_include_directories(platcheck_lib PUBLIC platcheck/include)

add_executable(platcheck platcheck/src/main.cpp)
target_link_libraries(platcheck PRIVATE platcheck_lib)

if(BUILD_TESTING_SUITE)
    find_package(GTest REQUIRED)
    add_executable(platcheck_tests platcheck/tests/test_checks.cpp)
    target_link_libraries(platcheck_tests PRIVATE platcheck_lib GTest::gtest_main)
    add_test(NAME platcheck_unit COMMAND platcheck_tests)
endif()

# ---- vmtest: KVM/QEMU validation harness -------------------------------------
if(BUILD_VMTEST)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBVIRT REQUIRED IMPORTED_TARGET libvirt libvirt-qemu)
    find_package(nlohmann_json 3.2.0 REQUIRED)

    add_library(vmtest_lib
        vmtest/src/util.cpp
        vmtest/src/junit.cpp
        vmtest/src/config.cpp
        vmtest/src/virt.cpp
        vmtest/src/guest_exec.cpp
        vmtest/src/tests.cpp)
    target_include_directories(vmtest_lib PUBLIC vmtest/include)
    target_link_libraries(vmtest_lib PUBLIC PkgConfig::LIBVIRT nlohmann_json::nlohmann_json)

    add_executable(vmtest vmtest/src/main.cpp)
    target_link_libraries(vmtest PRIVATE vmtest_lib)

    if(BUILD_TESTING_SUITE)
        # Pure-logic tests for the harness (no libvirt daemon or VM required).
        add_executable(vmtest_unit_tests vmtest/tests/test_harness.cpp)
        target_link_libraries(vmtest_unit_tests PRIVATE vmtest_lib GTest::gtest_main)
        add_test(NAME vmtest_unit COMMAND vmtest_unit_tests)
    endif()
endif()
