// CUDA Driver API behaviour: initialisation, contexts, memory, error reporting, PTX module
// loading, and interoperation with the Runtime API.
//
// Needs a GPU; skipped without one (failed with CUDATEST_REQUIRE_GPU=1).

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cudatest/reference.hpp"
#include "cudatest/testing.hpp"

using namespace cudatest;

#define CU_ASSERT_OK(call)                                                                       \
    do {                                                                                         \
        const CUresult cudatest_r_ = (call);                                                     \
        const char* cudatest_name_ = "?";                                                        \
        cuGetErrorName(cudatest_r_, &cudatest_name_);                                            \
        ASSERT_EQ(cudatest_r_, CUDA_SUCCESS) << #call << " -> " << cudatest_name_;               \
    } while (0)

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

class DriverApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        const CUresult r = cuInit(0);
        int n = 0;
        std::string why;
        if (r != CUDA_SUCCESS) {
            const char* name = "?";
            cuGetErrorName(r, &name);
            why = std::string("cuInit failed: ") + name;
        } else if (cuDeviceGetCount(&n) != CUDA_SUCCESS || n == 0) {
            why = "no CUDA devices found";
        }
        if (!why.empty()) {
            if (require_gpu()) FAIL() << "CUDATEST_REQUIRE_GPU is set but no GPU is usable: " << why;
            GTEST_SKIP() << why;
        }
        CU_ASSERT_OK(cuDeviceGet(&dev_, 0));
        // The primary context is the one the Runtime API uses, so both APIs see the same memory.
        CU_ASSERT_OK(cuDevicePrimaryCtxRetain(&ctx_, dev_));
        CU_ASSERT_OK(cuCtxSetCurrent(ctx_));
    }
    void TearDown() override {
        if (ctx_) cuDevicePrimaryCtxRelease(dev_);
    }

    CUdevice dev_ = 0;
    CUcontext ctx_ = nullptr;
};

TEST_F(DriverApiTest, DriverAndRuntimeAgreeAboutTheDevice) {
    char name[256] = {0};
    CU_ASSERT_OK(cuDeviceGetName(name, sizeof name, dev_));
    size_t total = 0;
    CU_ASSERT_OK(cuDeviceTotalMem(&total, dev_));
    int major = 0, minor = 0;
    CU_ASSERT_OK(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev_));
    CU_ASSERT_OK(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev_));

    cudaDeviceProp p;
    CUDA_ASSERT_OK(cudaGetDeviceProperties(&p, 0));
    EXPECT_GT(std::strlen(name), 0u);
    EXPECT_STREQ(name, p.name);
    EXPECT_EQ(total, p.totalGlobalMem);
    EXPECT_EQ(major, p.major);
    EXPECT_EQ(minor, p.minor);
}

TEST_F(DriverApiTest, DriverVersionMatchesTheRuntimesView) {
    int a = 0, b = 0;
    CU_ASSERT_OK(cuDriverGetVersion(&a));
    CUDA_ASSERT_OK(cudaDriverGetVersion(&b));
    EXPECT_EQ(a, b);
    EXPECT_GT(a, 0);
}

TEST_F(DriverApiTest, MemoryAllocationAndCopiesRoundTrip) {
    const size_t n = 1 << 16;
    const auto data = random_bytes(n, 31);
    CUdeviceptr d = 0;
    CU_ASSERT_OK(cuMemAlloc(&d, n));
    CU_ASSERT_OK(cuMemcpyHtoD(d, data.data(), n));
    std::vector<uint8_t> back(n, 0);
    CU_ASSERT_OK(cuMemcpyDtoH(back.data(), d, n));
    EXPECT_EQ(back, data);
    CU_ASSERT_OK(cuMemFree(d));
}

TEST_F(DriverApiTest, MemGetInfoIsConsistent) {
    size_t free_b = 0, total_b = 0;
    CU_ASSERT_OK(cuMemGetInfo(&free_b, &total_b));
    EXPECT_GT(total_b, 0u);
    EXPECT_LE(free_b, total_b);
}

TEST_F(DriverApiTest, InvalidMemoryOperationsReturnErrorsInsteadOfCrashing) {
    CUdeviceptr d = 0;
    EXPECT_EQ(cuMemAlloc(&d, 0), CUDA_ERROR_INVALID_VALUE) << "zero-byte allocation";
    EXPECT_NE(cuMemFree(static_cast<CUdeviceptr>(0xDEADBEEF)), CUDA_SUCCESS) << "freeing a bogus pointer";

    // The context is still usable afterwards.
    CU_ASSERT_OK(cuMemAlloc(&d, 256));
    CU_ASSERT_OK(cuMemFree(d));
}

TEST_F(DriverApiTest, ErrorNamesAndStringsAreReported) {
    const char* name = nullptr;
    const char* text = nullptr;
    CU_ASSERT_OK(cuGetErrorName(CUDA_ERROR_INVALID_VALUE, &name));
    CU_ASSERT_OK(cuGetErrorString(CUDA_ERROR_INVALID_VALUE, &text));
    EXPECT_STREQ(name, "CUDA_ERROR_INVALID_VALUE");
    ASSERT_NE(text, nullptr);
    EXPECT_GT(std::strlen(text), 0u);

    EXPECT_EQ(cuGetErrorName(static_cast<CUresult>(0x7FFFFFFF), &name), CUDA_ERROR_INVALID_VALUE)
        << "an unknown error code must itself be rejected";
}

TEST_F(DriverApiTest, PtxModuleLoadsAndAKernelLaunchesThroughCuLaunchKernel) {
#ifndef CUDATEST_PTX_PATH
    GTEST_SKIP() << "CUDATEST_PTX_PATH was not defined at build time";
#else
    const std::string ptx = read_file(CUDATEST_PTX_PATH);
    ASSERT_FALSE(ptx.empty()) << "cannot read " << CUDATEST_PTX_PATH;

    CUmodule mod = nullptr;
    CU_ASSERT_OK(cuModuleLoadData(&mod, ptx.c_str()));  // the driver JIT-compiles PTX for this GPU
    CUfunction fn = nullptr;
    CU_ASSERT_OK(cuModuleGetFunction(&fn, mod, "add_one"));

    const unsigned n = 1000;
    std::vector<uint32_t> host(n);
    for (unsigned i = 0; i < n; ++i) host[i] = i * 3;
    CUdeviceptr d = 0;
    CU_ASSERT_OK(cuMemAlloc(&d, n * sizeof(uint32_t)));
    CU_ASSERT_OK(cuMemcpyHtoD(d, host.data(), n * sizeof(uint32_t)));

    unsigned count = n;
    void* args[] = {&d, &count};
    const unsigned block = 256, grid = (n + block - 1) / block;
    CU_ASSERT_OK(cuLaunchKernel(fn, grid, 1, 1, block, 1, 1, 0, nullptr, args, nullptr));
    CU_ASSERT_OK(cuCtxSynchronize());

    std::vector<uint32_t> out(n);
    CU_ASSERT_OK(cuMemcpyDtoH(out.data(), d, n * sizeof(uint32_t)));
    for (unsigned i = 0; i < n; ++i) ASSERT_EQ(out[i], i * 3 + 1) << "element " << i;

    CU_ASSERT_OK(cuMemFree(d));
    CU_ASSERT_OK(cuModuleUnload(mod));
#endif
}

TEST_F(DriverApiTest, ModuleErrorsAreReported) {
#ifndef CUDATEST_PTX_PATH
    GTEST_SKIP() << "CUDATEST_PTX_PATH was not defined at build time";
#else
    const std::string ptx = read_file(CUDATEST_PTX_PATH);
    CUmodule mod = nullptr;
    CU_ASSERT_OK(cuModuleLoadData(&mod, ptx.c_str()));
    CUfunction fn = nullptr;
    EXPECT_EQ(cuModuleGetFunction(&fn, mod, "no_such_kernel"), CUDA_ERROR_NOT_FOUND);
    CU_ASSERT_OK(cuModuleUnload(mod));

    CUmodule bad = nullptr;
    EXPECT_NE(cuModuleLoadData(&bad, "this is not PTX or a cubin"), CUDA_SUCCESS);
#endif
}

TEST_F(DriverApiTest, RuntimeAllocationsAreUsableFromTheDriverApi) {
    // Both APIs share the primary context, so a pointer from cudaMalloc is valid in cuMemcpy*.
    const std::vector<uint32_t> data = {11, 22, 33, 44};
    void* p = nullptr;
    CUDA_ASSERT_OK(cudaMalloc(&p, data.size() * sizeof(uint32_t)));
    CUDA_ASSERT_OK(cudaMemcpy(p, data.data(), data.size() * sizeof(uint32_t), cudaMemcpyHostToDevice));

    std::vector<uint32_t> back(data.size(), 0);
    CU_ASSERT_OK(cuMemcpyDtoH(back.data(), reinterpret_cast<CUdeviceptr>(p), back.size() * sizeof(uint32_t)));
    EXPECT_EQ(back, data);
    CUDA_ASSERT_OK(cudaFree(p));
}
