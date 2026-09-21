cmake_minimum_required(VERSION 3.20)
project(kvm_validation_suite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTING_SUITE "Build unit tests (needs GoogleTest)" ON)
option(BUILD_VMTEST "Build the libvirt-based VM test harness (needs libvirt + nlohmann-json)" ON)
option(BUILD_CUDATEST "Build the CUDA test suite (host-only reference tests always; GPU tests if the CUDA toolkit is found)" ON)
option(BUILD_DTEST "Build the distributed test runner (core needs nlohmann-json; agent/controller need gRPC)" ON)

if(BUILD_VMTEST OR BUILD_DTEST)
    find_package(nlohmann_json 3.2.0 REQUIRED)
endif()

# Host-compiler warnings only for C++ (nvcc does not understand these flags).
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall$<SEMICOLON>-Wextra$<SEMICOLON>-Wpedantic$<SEMICOLON>-Wshadow>)

enable_testing()
find_package(Threads REQUIRED)

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

# ---- dtest: distributed test runner (agent + controller) ----------------------
if(BUILD_DTEST)
    # Core: process runner, allow-list policy, job parsing, scheduler, reports. No gRPC needed.
    add_library(dtest_core
        dtest/src/process.cpp
        dtest/src/policy.cpp
        dtest/src/jobs.cpp
        dtest/src/scheduler.cpp
        dtest/src/report.cpp)
    target_include_directories(dtest_core PUBLIC dtest/include)
    target_link_libraries(dtest_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

    if(BUILD_TESTING_SUITE)
        add_executable(dtest_core_tests
            dtest/tests/test_process.cpp
            dtest/tests/test_policy.cpp
            dtest/tests/test_jobs.cpp
            dtest/tests/test_scheduler.cpp)
        target_link_libraries(dtest_core_tests PRIVATE dtest_core GTest::gtest_main)
        add_test(NAME dtest_core_unit COMMAND dtest_core_tests)
    endif()

    # Network layer: needs protobuf + gRPC.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP IMPORTED_TARGET grpc++ protobuf)
    find_program(DTEST_PROTOC protoc)
    find_program(DTEST_GRPC_PLUGIN grpc_cpp_plugin)

    if(GRPCPP_FOUND AND DTEST_PROTOC AND DTEST_GRPC_PLUGIN)
        set(DTEST_PROTO ${CMAKE_SOURCE_DIR}/dtest/proto/dtest.proto)
        set(DTEST_GEN_DIR ${CMAKE_BINARY_DIR}/dtest_gen)
        file(MAKE_DIRECTORY ${DTEST_GEN_DIR})
        add_custom_command(
            OUTPUT ${DTEST_GEN_DIR}/dtest.pb.cc ${DTEST_GEN_DIR}/dtest.pb.h
                   ${DTEST_GEN_DIR}/dtest.grpc.pb.cc ${DTEST_GEN_DIR}/dtest.grpc.pb.h
            COMMAND ${DTEST_PROTOC}
                    --proto_path=${CMAKE_SOURCE_DIR}/dtest/proto
                    --cpp_out=${DTEST_GEN_DIR}
                    --grpc_out=${DTEST_GEN_DIR}
                    --plugin=protoc-gen-grpc=${DTEST_GRPC_PLUGIN}
                    ${DTEST_PROTO}
            DEPENDS ${DTEST_PROTO}
            COMMENT "Generating gRPC/protobuf sources for dtest")

        add_library(dtest_net
            ${DTEST_GEN_DIR}/dtest.pb.cc
            ${DTEST_GEN_DIR}/dtest.grpc.pb.cc
            dtest/src/agent_service.cpp
            dtest/src/grpc_executor.cpp)
        target_include_directories(dtest_net PUBLIC ${DTEST_GEN_DIR})
        target_link_libraries(dtest_net PUBLIC dtest_core PkgConfig::GRPCPP)
        # Generated code is not ours to lint.
        set_source_files_properties(${DTEST_GEN_DIR}/dtest.pb.cc ${DTEST_GEN_DIR}/dtest.grpc.pb.cc
            PROPERTIES COMPILE_FLAGS "-w")

        add_executable(dtest-agent dtest/src/agent_main.cpp)
        target_link_libraries(dtest-agent PRIVATE dtest_net)
        add_executable(dtest-ctl dtest/src/ctl_main.cpp)
        target_link_libraries(dtest-ctl PRIVATE dtest_net)

        if(BUILD_TESTING_SUITE)
            add_executable(dtest_net_tests dtest/tests/test_grpc_integration.cpp)
            target_link_libraries(dtest_net_tests PRIVATE dtest_net GTest::gtest_main)
            add_test(NAME dtest_net_integration COMMAND dtest_net_tests)

            add_test(NAME dtest_e2e
                COMMAND ${CMAKE_SOURCE_DIR}/dtest/tests/e2e.sh $<TARGET_FILE:dtest-agent> $<TARGET_FILE:dtest-ctl>)
        endif()
    else()
        message(WARNING "gRPC/protobuf not found: building dtest core only (no agent/controller). "
                        "Install libgrpc++-dev, libprotobuf-dev, protobuf-compiler, protobuf-compiler-grpc.")
    endif()
endif()

# ---- cudatest: CUDA test suite -------------------------------------------------------------
if(BUILD_CUDATEST)
    # The oracle (CPU references + comparison helpers) needs no CUDA and is always built and tested.
    add_library(cudatest_reference cudatest/src/reference.cpp)
    target_include_directories(cudatest_reference PUBLIC cudatest/include)

    if(BUILD_TESTING_SUITE)
        add_executable(cudatest_reference_tests cudatest/tests/test_reference.cpp)
        target_link_libraries(cudatest_reference_tests PRIVATE cudatest_reference GTest::gtest_main)
        add_test(NAME cudatest_reference_unit COMMAND cudatest_reference_tests)
    endif()

    # nvcc 12.0-12.3 rejects GCC 13; prefer an older host compiler for CUDA when one is installed.
    if(NOT DEFINED CMAKE_CUDA_HOST_COMPILER AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
        find_program(CUDATEST_OLD_GXX NAMES g++-12 g++-11 g++-10)
        if(CUDATEST_OLD_GXX)
            set(CMAKE_CUDA_HOST_COMPILER ${CUDATEST_OLD_GXX})
        endif()
    endif()
    # Real GPUs from Turing to Hopper (each also embeds PTX, so newer GPUs JIT-compile it). Override with
    # -DCMAKE_CUDA_ARCHITECTURES=native (CMake >= 3.24) to build only for the local GPU.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "75;80;86;89;90")
    endif()

    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        find_package(CUDAToolkit REQUIRED)

        add_library(cudatest_kernels cudatest/src/kernels.cu)
        set_target_properties(cudatest_kernels PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
        target_include_directories(cudatest_kernels PUBLIC cudatest/include)
        target_link_libraries(cudatest_kernels PUBLIC CUDA::cudart)
        target_compile_options(cudatest_kernels PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall$<COMMA>-Wextra>)

        find_program(CUDATEST_PTXAS ptxas HINTS ${CUDAToolkit_BIN_DIR})

        if(BUILD_TESTING_SUITE)
            add_executable(cudatest_gpu_tests
                cudatest/tests/test_kernels.cpp
                cudatest/tests/test_runtime_api.cpp
                cudatest/tests/test_driver_api.cpp)
            target_compile_definitions(cudatest_gpu_tests PRIVATE
                CUDATEST_PTX_PATH="${CMAKE_SOURCE_DIR}/cudatest/ptx/add_one.ptx")
            target_link_libraries(cudatest_gpu_tests PRIVATE
                cudatest_kernels cudatest_reference CUDA::cudart CUDA::cuda_driver
                GTest::gtest_main Threads::Threads)
            # Skips (exit code 0) when no GPU is present; set CUDATEST_REQUIRE_GPU=1 to make that a failure.
            add_test(NAME cudatest_gpu COMMAND cudatest_gpu_tests)

            if(CUDATEST_PTXAS)
                add_test(NAME cudatest_ptx_assembles
                    COMMAND ${CUDATEST_PTXAS} -arch=sm_52 -o /dev/null ${CMAKE_SOURCE_DIR}/cudatest/ptx/add_one.ptx)
            endif()
        endif()
    else()
        message(WARNING "CUDA compiler not found: building only the host-side cudatest reference library and tests.")
    endif()
endif()
