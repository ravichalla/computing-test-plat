// CUDA Runtime API behaviour: device properties, memory, streams, events, unified memory,
// multi-threaded use, and error handling (including the negative paths).
//
// Needs a GPU; skipped without one (failed with CUDATEST_REQUIRE_GPU=1).

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cudatest/device_buffer.hpp"
#include "cudatest/kernels.hpp"
#include "cudatest/reference.hpp"
#include "cudatest/testing.hpp"

using namespace cudatest;

class RuntimeApiTest : public GpuTest {};

// ---- device and version ---------------------------------------------------------------------

TEST_F(RuntimeApiTest, DevicePropertiesAreSane) {
    int n = 0;
    CUDA_ASSERT_OK(cudaGetDeviceCount(&n));
    ASSERT_GE(n, 1);

    cudaDeviceProp p;
    CUDA_ASSERT_OK(cudaGetDeviceProperties(&p, 0));
    EXPECT_GT(std::strlen(p.name), 0u);
    EXPECT_GT(p.totalGlobalMem, 0u);
    EXPECT_EQ(p.warpSize, 32);
    EXPECT_GE(p.maxThreadsPerBlock, 512);
    EXPECT_GE(p.major, 3);
    EXPECT_GT(p.multiProcessorCount, 0);
    EXPECT_GE(p.sharedMemPerBlock, 16u * 1024u);
}

TEST_F(RuntimeApiTest, RuntimeAndDriverVersionsAreCompatible) {
    int rt = 0, drv = 0;
    CUDA_ASSERT_OK(cudaRuntimeGetVersion(&rt));
    CUDA_ASSERT_OK(cudaDriverGetVersion(&drv));
    EXPECT_GT(rt, 0);
    EXPECT_GT(drv, 0);
    // Minor-version compatibility lets a newer runtime run on an older driver of the same
    // major release, so only the major version has to line up.
    EXPECT_GE(drv / 1000, rt / 1000) << "driver " << drv << " is older than runtime " << rt;
}

TEST_F(RuntimeApiTest, MemGetInfoIsConsistentAcrossAnAllocation) {
    size_t free0 = 0, total0 = 0, free1 = 0, total1 = 0;
    CUDA_ASSERT_OK(cudaMemGetInfo(&free0, &total0));
    EXPECT_GT(total0, 0u);
    EXPECT_LE(free0, total0);
    {
        DeviceBuffer<char> big(64u << 20);
        CUDA_ASSERT_OK(cudaMemGetInfo(&free1, &total1));
        EXPECT_EQ(total0, total1);
        EXPECT_LE(free1, free0) << "free memory cannot grow while a 64 MiB buffer is live";
    }
}

// ---- memory ---------------------------------------------------------------------------------

class MemcpyTest : public GpuFixture<::testing::TestWithParam<size_t>> {};

TEST_P(MemcpyTest, HostToDeviceToHostRoundTripIsExact) {
    const size_t n = GetParam();
    const auto data = random_bytes(n, 21);
    DeviceBuffer<uint8_t> d(data);
    EXPECT_EQ(d.download(), data);
}

INSTANTIATE_TEST_SUITE_P(Sizes, MemcpyTest, ::testing::Values<size_t>(1, 255, 4096, 1 << 20, (1 << 24) + 5),
                         [](const ::testing::TestParamInfo<size_t>& i) { return "bytes" + std::to_string(i.param); });

TEST_F(RuntimeApiTest, MemsetAndDeviceToDeviceCopy) {
    const size_t n = 4096;
    DeviceBuffer<uint8_t> a(n), b(n);
    a.fill_bytes(0x5A);
    CUDA_ASSERT_OK(cudaMemcpy(b.get(), a.get(), n, cudaMemcpyDeviceToDevice));
    for (uint8_t x : b.download()) ASSERT_EQ(x, 0x5A);
}

TEST_F(RuntimeApiTest, PinnedMemoryAsyncRoundTripThroughAStream) {
    const size_t n = 1 << 20;
    const auto data = random_bytes(n, 22);

    uint8_t* pin_in = nullptr;
    uint8_t* pin_out = nullptr;
    CUDA_ASSERT_OK(cudaHostAlloc(reinterpret_cast<void**>(&pin_in), n, cudaHostAllocDefault));
    CUDA_ASSERT_OK(cudaHostAlloc(reinterpret_cast<void**>(&pin_out), n, cudaHostAllocDefault));
    std::memcpy(pin_in, data.data(), n);
    std::memset(pin_out, 0, n);

    DeviceBuffer<uint8_t> d(n);
    cudaStream_t s;
    CUDA_ASSERT_OK(cudaStreamCreate(&s));
    CUDA_ASSERT_OK(cudaMemcpyAsync(d.get(), pin_in, n, cudaMemcpyHostToDevice, s));
    CUDA_ASSERT_OK(cudaMemcpyAsync(pin_out, d.get(), n, cudaMemcpyDeviceToHost, s));
    CUDA_ASSERT_OK(cudaStreamSynchronize(s));
    EXPECT_EQ(cudaStreamQuery(s), cudaSuccess) << "a synchronised stream must report no pending work";
    EXPECT_EQ(std::memcmp(pin_out, data.data(), n), 0);

    CUDA_ASSERT_OK(cudaStreamDestroy(s));
    CUDA_ASSERT_OK(cudaFreeHost(pin_in));
    CUDA_ASSERT_OK(cudaFreeHost(pin_out));
}

TEST_F(RuntimeApiTest, UnifiedMemoryIsVisibleToTheHostAndToKernels) {
    int managed = 0;
    CUDA_ASSERT_OK(cudaDeviceGetAttribute(&managed, cudaDevAttrManagedMemory, 0));
    if (!managed) GTEST_SKIP() << "device does not support managed memory";

    const int n = 4096;
    float *a = nullptr, *b = nullptr, *c = nullptr;
    CUDA_ASSERT_OK(cudaMallocManaged(reinterpret_cast<void**>(&a), n * sizeof(float)));
    CUDA_ASSERT_OK(cudaMallocManaged(reinterpret_cast<void**>(&b), n * sizeof(float)));
    CUDA_ASSERT_OK(cudaMallocManaged(reinterpret_cast<void**>(&c), n * sizeof(float)));
    for (int i = 0; i < n; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = 0.5f;
    }
    CUDA_ASSERT_OK(launch_vector_add(a, b, c, n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    for (int i = 0; i < n; ++i) ASSERT_EQ(c[i], static_cast<float>(i) + 0.5f) << i;

    CUDA_ASSERT_OK(cudaFree(a));
    CUDA_ASSERT_OK(cudaFree(b));
    CUDA_ASSERT_OK(cudaFree(c));
}

// ---- streams and events ---------------------------------------------------------------------

TEST_F(RuntimeApiTest, EventsMeasureNonNegativeElapsedTime) {
    const int n = 1 << 22;
    DeviceBuffer<float> a(std::vector<float>(n, 1.0f)), b(std::vector<float>(n, 2.0f)), c(n);
    cudaStream_t s;
    cudaEvent_t start, stop;
    CUDA_ASSERT_OK(cudaStreamCreate(&s));
    CUDA_ASSERT_OK(cudaEventCreate(&start));
    CUDA_ASSERT_OK(cudaEventCreate(&stop));

    CUDA_ASSERT_OK(cudaEventRecord(start, s));
    CUDA_ASSERT_OK(launch_vector_add(a.get(), b.get(), c.get(), n, 256, s));
    CUDA_ASSERT_OK(cudaEventRecord(stop, s));
    CUDA_ASSERT_OK(cudaEventSynchronize(stop));

    float ms = -1.0f;
    CUDA_ASSERT_OK(cudaEventElapsedTime(&ms, start, stop));
    EXPECT_GE(ms, 0.0f);
    EXPECT_EQ(cudaEventQuery(stop), cudaSuccess);

    CUDA_ASSERT_OK(cudaEventDestroy(start));
    CUDA_ASSERT_OK(cudaEventDestroy(stop));
    CUDA_ASSERT_OK(cudaStreamDestroy(s));
}

TEST_F(RuntimeApiTest, StreamWaitEventOrdersDependentWorkAcrossStreams) {
    const int n = 1 << 18;
    const auto a = random_floats(n, 23), b = random_floats(n, 24);
    std::vector<float> ab(n), expected(n);
    cpu_vector_add(a.data(), b.data(), ab.data(), n);
    cpu_vector_add(ab.data(), ab.data(), expected.data(), n);  // (a+b) + (a+b)

    DeviceBuffer<float> da(a), db(b), dab(n), dout(n);
    cudaStream_t s1, s2;
    cudaEvent_t done;
    CUDA_ASSERT_OK(cudaStreamCreate(&s1));
    CUDA_ASSERT_OK(cudaStreamCreate(&s2));
    CUDA_ASSERT_OK(cudaEventCreate(&done));

    CUDA_ASSERT_OK(launch_vector_add(da.get(), db.get(), dab.get(), n, 256, s1));  // stage 1 on s1
    CUDA_ASSERT_OK(cudaEventRecord(done, s1));
    CUDA_ASSERT_OK(cudaStreamWaitEvent(s2, done, 0));                              // s2 must not start early
    CUDA_ASSERT_OK(launch_vector_add(dab.get(), dab.get(), dout.get(), n, 256, s2));  // stage 2 on s2
    CUDA_ASSERT_OK(cudaStreamSynchronize(s2));

    const auto r = allclose(dout.download(), expected, 0, 0);
    EXPECT_TRUE(r.ok) << r.message;

    CUDA_ASSERT_OK(cudaEventDestroy(done));
    CUDA_ASSERT_OK(cudaStreamDestroy(s1));
    CUDA_ASSERT_OK(cudaStreamDestroy(s2));
}

TEST_F(RuntimeApiTest, ManyHostThreadsCanShareTheDevice) {
    constexpr int kThreads = 8;
    std::atomic<int> ok{0};
    std::mutex mu;
    std::vector<std::string> failures;

    auto worker = [&](int id) {
        try {
            cuda_check(cudaSetDevice(0), "cudaSetDevice");
            const int n = 100000 + id;
            const auto a = random_floats(n, 100 + id), b = random_floats(n, 200 + id);
            std::vector<float> expected(n);
            cpu_vector_add(a.data(), b.data(), expected.data(), n);

            cudaStream_t s;
            cuda_check(cudaStreamCreate(&s), "cudaStreamCreate");
            DeviceBuffer<float> da(a), db(b), dc(n);
            for (int rep = 0; rep < 20; ++rep) {
                cuda_check(launch_vector_add(da.get(), db.get(), dc.get(), n, 256, s), "launch");
            }
            cuda_check(cudaStreamSynchronize(s), "sync");
            const auto r = allclose(dc.download(), expected, 0, 0);
            cuda_check(cudaStreamDestroy(s), "cudaStreamDestroy");
            if (r.ok) {
                ++ok;
            } else {
                std::lock_guard<std::mutex> lk(mu);
                failures.push_back("thread " + std::to_string(id) + ": " + r.message);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mu);
            failures.push_back("thread " + std::to_string(id) + ": " + e.what());
        }
    };

    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) ts.emplace_back(worker, i);
    for (auto& t : ts) t.join();

    EXPECT_EQ(ok.load(), kThreads);
    for (const auto& f : failures) ADD_FAILURE() << f;
}

// ---- error handling -------------------------------------------------------------------------

class RuntimeErrorTest : public GpuTest {};

TEST_F(RuntimeErrorTest, AnImpossibleAllocationFailsCleanlyAndTheDeviceStaysUsable) {
    void* p = nullptr;
    const cudaError_t e = cudaMalloc(&p, std::numeric_limits<size_t>::max() / 2);
    EXPECT_TRUE(e == cudaErrorMemoryAllocation || e == cudaErrorInvalidValue) << cudaGetErrorName(e);
    EXPECT_EQ(p, nullptr);

    EXPECT_EQ(cudaGetLastError(), e) << "the failing call must be recorded as the last error";
    EXPECT_EQ(cudaGetLastError(), cudaSuccess) << "reading the last error must clear it";

    // Non-sticky: normal work still succeeds afterwards.
    CUDA_ASSERT_OK(cudaMalloc(&p, 1024));
    CUDA_ASSERT_OK(cudaFree(p));
}

TEST_F(RuntimeErrorTest, InvalidPointersAreRejectedWithoutBreakingTheContext) {
    int host_local = 0;
    EXPECT_EQ(cudaFree(&host_local), cudaErrorInvalidValue) << "freeing a host pointer";
    cudaGetLastError();

    void* p = nullptr;
    CUDA_ASSERT_OK(cudaMalloc(&p, 64));
    CUDA_ASSERT_OK(cudaFree(p));
    EXPECT_EQ(cudaFree(p), cudaErrorInvalidValue) << "double free";
    cudaGetLastError();

    CUDA_EXPECT_OK(cudaFree(nullptr));  // documented no-op

    EXPECT_EQ(cudaMemcpy(nullptr, &host_local, sizeof host_local, cudaMemcpyHostToDevice), cudaErrorInvalidValue);
    cudaGetLastError();

    // Still healthy.
    DeviceBuffer<int> d(std::vector<int>{1, 2, 3});
    EXPECT_EQ(d.download(), (std::vector<int>{1, 2, 3}));
}

TEST_F(RuntimeErrorTest, InvalidDeviceOrdinalsAreRejected) {
    int n = 0;
    CUDA_ASSERT_OK(cudaGetDeviceCount(&n));
    EXPECT_EQ(cudaSetDevice(n), cudaErrorInvalidDevice);
    EXPECT_EQ(cudaSetDevice(-1), cudaErrorInvalidDevice);
    cudaGetLastError();
    CUDA_EXPECT_OK(cudaSetDevice(0));
}

TEST_F(RuntimeErrorTest, ErrorNamesAndStringsAreDefinedForCommonCodes) {
    for (cudaError_t e : {cudaSuccess, cudaErrorMemoryAllocation, cudaErrorInvalidValue, cudaErrorInvalidConfiguration,
                          cudaErrorInvalidDevice}) {
        ASSERT_NE(cudaGetErrorName(e), nullptr);
        ASSERT_NE(cudaGetErrorString(e), nullptr);
        EXPECT_GT(std::strlen(cudaGetErrorName(e)), 0u);
        EXPECT_GT(std::strlen(cudaGetErrorString(e)), 0u);
    }
    EXPECT_STREQ(cudaGetErrorName(cudaErrorMemoryAllocation), "cudaErrorMemoryAllocation");
}

TEST_F(RuntimeErrorTest, TheDeviceIsHealthyAtTheEndOfTheRun) {
    CUDA_EXPECT_OK(cudaDeviceSynchronize());
    CUDA_EXPECT_OK(cudaGetLastError());
}
