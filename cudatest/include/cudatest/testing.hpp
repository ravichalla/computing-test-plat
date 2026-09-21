// gtest helpers for GPU tests.
#pragma once

#include <cuda_runtime_api.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#define CUDA_ASSERT_OK(call)                                                                        \
    do {                                                                                            \
        const cudaError_t cudatest_e_ = (call);                                                     \
        ASSERT_EQ(cudatest_e_, cudaSuccess) << #call << " -> " << cudaGetErrorName(cudatest_e_)    \
                                            << ": " << cudaGetErrorString(cudatest_e_);             \
    } while (0)

#define CUDA_EXPECT_OK(call)                                                                        \
    do {                                                                                            \
        const cudaError_t cudatest_e_ = (call);                                                     \
        EXPECT_EQ(cudatest_e_, cudaSuccess) << #call << " -> " << cudaGetErrorName(cudatest_e_)    \
                                            << ": " << cudaGetErrorString(cudatest_e_);             \
    } while (0)

namespace cudatest {

// CI on a GPU machine should not silently skip everything: set CUDATEST_REQUIRE_GPU=1 to turn
// "no usable GPU" from a skip into a failure.
inline bool require_gpu() {
    const char* v = std::getenv("CUDATEST_REQUIRE_GPU");
    return v && *v && std::string(v) != "0";
}

// Empty string if a GPU is usable, otherwise the reason it is not.
inline std::string gpu_unavailable_reason() {
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) {
        cudaGetLastError();  // clear the recorded error so later tests start clean
        return std::string("cudaGetDeviceCount failed: ") + cudaGetErrorString(e);
    }
    if (n == 0) return "no CUDA devices found";
    return "";
}

// Fixture mixin: skips (or fails, with CUDATEST_REQUIRE_GPU) when there is no GPU, and starts
// every test on device 0 with a clear error state.
template <class Base>
class GpuFixture : public Base {
protected:
    void SetUp() override {
        const std::string why = gpu_unavailable_reason();
        if (!why.empty()) {
            if (require_gpu()) FAIL() << "CUDATEST_REQUIRE_GPU is set but no GPU is usable: " << why;
            GTEST_SKIP() << why;
        }
        ASSERT_EQ(cudaSetDevice(0), cudaSuccess);
        cudaGetLastError();
    }
};

using GpuTest = GpuFixture<::testing::Test>;

}  // namespace cudatest
