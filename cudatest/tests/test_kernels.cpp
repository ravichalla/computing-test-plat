// Correctness tests for the CUDA kernels: every GPU result is compared with a CPU reference
// (or, for GEMM, with two independent implementations).
//
// These need a GPU; without one they are skipped (or failed, with CUDATEST_REQUIRE_GPU=1).

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "cudatest/device_buffer.hpp"
#include "cudatest/kernels.hpp"
#include "cudatest/reference.hpp"
#include "cudatest/testing.hpp"

using namespace cudatest;

namespace {

// Sizes chosen around warp (32), block (256) and grid boundaries, plus a large non-round one.
const std::vector<int> kSizes = {1, 31, 32, 33, 255, 256, 257, 1023, 1025, (1 << 20) + 3};

std::string size_name(const ::testing::TestParamInfo<int>& i) { return "n" + std::to_string(i.param); }

}  // namespace

// =================================================================================================
// vector add
// =================================================================================================

class VectorAddTest : public GpuFixture<::testing::TestWithParam<int>> {};

TEST_P(VectorAddTest, MatchesTheCpuReferenceBitForBit) {
    const int n = GetParam();
    const auto a = random_floats(n, 1), b = random_floats(n, 2);
    std::vector<float> expected(n);
    cpu_vector_add(a.data(), b.data(), expected.data(), n);

    DeviceBuffer<float> da(a), db(b), dc(n);
    CUDA_ASSERT_OK(launch_vector_add(da.get(), db.get(), dc.get(), n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    const auto r = allclose(dc.download(), expected, 0, 0);
    EXPECT_TRUE(r.ok) << r.message;
}

INSTANTIATE_TEST_SUITE_P(Sizes, VectorAddTest, ::testing::ValuesIn(kSizes), size_name);

class VectorAddEdgeTest : public GpuTest {};

TEST_F(VectorAddEdgeTest, ResultDoesNotDependOnTheBlockSize) {
    const int n = 10007;
    const auto a = random_floats(n, 3), b = random_floats(n, 4);
    std::vector<float> expected(n);
    cpu_vector_add(a.data(), b.data(), expected.data(), n);
    DeviceBuffer<float> da(a), db(b), dc(n);

    for (int block : {32, 64, 128, 256, 512, 1024}) {
        dc.fill_bytes(0);
        CUDA_ASSERT_OK(launch_vector_add(da.get(), db.get(), dc.get(), n, block, nullptr));
        CUDA_ASSERT_OK(cudaDeviceSynchronize());
        const auto r = allclose(dc.download(), expected, 0, 0);
        EXPECT_TRUE(r.ok) << "block=" << block << ": " << r.message;
    }
}

TEST_F(VectorAddEdgeTest, NeverWritesPastTheEndOfTheOutput) {
    const int n = 1000, guard = 64;
    const auto a = random_floats(n, 5), b = random_floats(n, 6);
    DeviceBuffer<float> da(a), db(b), dc(n + guard);
    dc.fill_bytes(0xAB);

    CUDA_ASSERT_OK(launch_vector_add(da.get(), db.get(), dc.get(), n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    const auto out = dc.download();
    for (int i = n; i < n + guard; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &out[i], sizeof bits);
        ASSERT_EQ(bits, 0xABABABABu) << "guard element " << i << " was overwritten";
    }
}

TEST_F(VectorAddEdgeTest, ZeroLengthIsASuccessfulNoOp) {
    CUDA_EXPECT_OK(launch_vector_add(nullptr, nullptr, nullptr, 0, 256, nullptr));
}

TEST_F(VectorAddEdgeTest, InvalidArgumentsAreReportedAndLaterLaunchesStillWork) {
    DeviceBuffer<float> a(std::vector<float>(64, 1.0f)), b(std::vector<float>(64, 2.0f)), c(64);

    EXPECT_EQ(launch_vector_add(a.get(), b.get(), c.get(), 64, 0, nullptr), cudaErrorInvalidConfiguration);
    EXPECT_EQ(launch_vector_add(a.get(), b.get(), c.get(), 64, 2048, nullptr), cudaErrorInvalidConfiguration);
    EXPECT_EQ(launch_vector_add(a.get(), b.get(), c.get(), -1, 256, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_vector_add(nullptr, b.get(), c.get(), 64, 256, nullptr), cudaErrorInvalidValue);

    // The bad launches must not leave a sticky error behind.
    CUDA_ASSERT_OK(cudaGetLastError());
    CUDA_ASSERT_OK(launch_vector_add(a.get(), b.get(), c.get(), 64, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    for (float x : c.download()) ASSERT_EQ(x, 3.0f);
}

// =================================================================================================
// reduction
// =================================================================================================

class ReduceTest : public GpuFixture<::testing::TestWithParam<int>> {};

TEST_P(ReduceTest, MatchesTheCpuSum) {
    const int n = GetParam();
    const auto data = random_u32(n, 7, 1000);
    DeviceBuffer<uint32_t> in(data);
    DeviceBuffer<unsigned long long> out(1);

    CUDA_ASSERT_OK(launch_reduce_sum(in.get(), out.get(), n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    EXPECT_EQ(out.download()[0], cpu_reduce_sum(data.data(), data.size()));
}

INSTANTIATE_TEST_SUITE_P(Sizes, ReduceTest, ::testing::ValuesIn(kSizes), size_name);

class ReduceEdgeTest : public GpuTest {};

TEST_F(ReduceEdgeTest, AccumulatesIn64BitsWithoutOverflow) {
    const int n = 1 << 16;
    const std::vector<uint32_t> data(n, 0xFFFFFFFFu);  // the sum needs ~48 bits
    DeviceBuffer<uint32_t> in(data);
    DeviceBuffer<unsigned long long> out(1);

    CUDA_ASSERT_OK(launch_reduce_sum(in.get(), out.get(), n, 1024, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    EXPECT_EQ(out.download()[0], static_cast<unsigned long long>(0xFFFFFFFFu) * n);
}

TEST_F(ReduceEdgeTest, ResultDoesNotDependOnTheBlockSize) {
    const int n = 100003;
    const auto data = random_u32(n, 8, 100000);
    const uint64_t expected = cpu_reduce_sum(data.data(), data.size());
    DeviceBuffer<uint32_t> in(data);
    DeviceBuffer<unsigned long long> out(1);

    for (int block : {32, 64, 128, 256, 512, 1024}) {
        CUDA_ASSERT_OK(launch_reduce_sum(in.get(), out.get(), n, block, nullptr));
        CUDA_ASSERT_OK(cudaDeviceSynchronize());
        EXPECT_EQ(out.download()[0], expected) << "block=" << block;
    }
}

TEST_F(ReduceEdgeTest, RepeatedLaunchesDoNotAccumulateInTheOutput) {
    const auto data = random_u32(5000, 9, 100);
    const uint64_t expected = cpu_reduce_sum(data.data(), data.size());
    DeviceBuffer<uint32_t> in(data);
    DeviceBuffer<unsigned long long> out(1);
    for (int i = 0; i < 3; ++i) {
        CUDA_ASSERT_OK(launch_reduce_sum(in.get(), out.get(), 5000, 256, nullptr));
        CUDA_ASSERT_OK(cudaDeviceSynchronize());
        EXPECT_EQ(out.download()[0], expected) << "launch " << i;
    }
}

TEST_F(ReduceEdgeTest, EmptyInputSumsToZeroAndBadBlockSizesAreRejected) {
    DeviceBuffer<unsigned long long> out(std::vector<unsigned long long>{123});
    CUDA_ASSERT_OK(launch_reduce_sum(nullptr, out.get(), 0, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    EXPECT_EQ(out.download()[0], 0ull);

    DeviceBuffer<uint32_t> in(std::vector<uint32_t>(64, 1));
    for (int bad : {0, 16, 31, 48, 100, 2048}) {
        EXPECT_EQ(launch_reduce_sum(in.get(), out.get(), 64, bad, nullptr), cudaErrorInvalidValue) << "block=" << bad;
    }
}

// =================================================================================================
// GEMM: two implementations plus the CPU reference (differential testing)
// =================================================================================================

struct Shape {
    int m, n, k;
};

class GemmTest : public GpuFixture<::testing::TestWithParam<Shape>> {};

TEST_P(GemmTest, NaiveAndTiledAgreeWithTheCpuReference) {
    const auto [M, N, K] = GetParam();
    const auto A = random_floats(static_cast<size_t>(M) * K, 10);
    const auto B = random_floats(static_cast<size_t>(K) * N, 11);
    std::vector<float> expected(static_cast<size_t>(M) * N);
    cpu_gemm(A.data(), B.data(), expected.data(), M, N, K);

    DeviceBuffer<float> dA(A), dB(B), naive(expected.size()), tiled(expected.size());
    CUDA_ASSERT_OK(launch_gemm_naive(dA.get(), dB.get(), naive.get(), M, N, K, nullptr));
    CUDA_ASSERT_OK(launch_gemm_tiled(dA.get(), dB.get(), tiled.get(), M, N, K, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    const auto rn = allclose(naive.download(), expected, 1e-3, 1e-3);
    const auto rt = allclose(tiled.download(), expected, 1e-3, 1e-3);
    EXPECT_TRUE(rn.ok) << "naive: " << rn.message;
    EXPECT_TRUE(rt.ok) << "tiled: " << rt.message;
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, GemmTest,
    ::testing::Values(Shape{1, 1, 1}, Shape{16, 16, 16}, Shape{17, 33, 19}, Shape{64, 64, 64}, Shape{100, 37, 250},
                      Shape{1, 128, 1}, Shape{128, 1, 64}, Shape{31, 32, 33}),
    [](const ::testing::TestParamInfo<Shape>& i) {
        return "M" + std::to_string(i.param.m) + "_N" + std::to_string(i.param.n) + "_K" + std::to_string(i.param.k);
    });

class GemmEdgeTest : public GpuTest {};

TEST_F(GemmEdgeTest, MultiplyingByTheIdentityReturnsTheInputExactly) {
    const int n = 50;
    const auto A = random_floats(n * n, 12);
    std::vector<float> I(n * n, 0.0f);
    for (int i = 0; i < n; ++i) I[i * n + i] = 1.0f;
    DeviceBuffer<float> dA(A), dI(I), dC(n * n);

    CUDA_ASSERT_OK(launch_gemm_tiled(dA.get(), dI.get(), dC.get(), n, n, n, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    const auto r = allclose(dC.download(), A, 0, 0);
    EXPECT_TRUE(r.ok) << r.message;
}

TEST_F(GemmEdgeTest, TheTiledKernelIsDeterministic) {
    const int M = 90, N = 70, K = 130;
    DeviceBuffer<float> dA(random_floats(M * K, 13)), dB(random_floats(K * N, 14)), c1(M * N), c2(M * N);
    CUDA_ASSERT_OK(launch_gemm_tiled(dA.get(), dB.get(), c1.get(), M, N, K, nullptr));
    CUDA_ASSERT_OK(launch_gemm_tiled(dA.get(), dB.get(), c2.get(), M, N, K, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    EXPECT_EQ(c1.download(), c2.download()) << "no atomics are involved, so results must be bit-identical";
}

TEST_F(GemmEdgeTest, InvalidAndEmptyArguments) {
    DeviceBuffer<float> a(16), b(16), c(16);
    EXPECT_EQ(launch_gemm_tiled(nullptr, b.get(), c.get(), 4, 4, 4, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_gemm_tiled(a.get(), b.get(), c.get(), -1, 4, 4, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_gemm_naive(a.get(), b.get(), c.get(), 4, 4, -4, nullptr), cudaErrorInvalidValue);
    CUDA_EXPECT_OK(launch_gemm_tiled(a.get(), b.get(), c.get(), 0, 4, 4, nullptr));  // empty output: no-op
}

// =================================================================================================
// histogram
// =================================================================================================

class HistogramTest : public GpuFixture<::testing::TestWithParam<int>> {};

TEST_P(HistogramTest, MatchesTheCpuHistogram) {
    const int n = GetParam();
    const auto data = random_bytes(n, 15);
    DeviceBuffer<uint8_t> in(data);
    DeviceBuffer<unsigned int> bins(256);

    CUDA_ASSERT_OK(launch_histogram256(in.get(), bins.get(), n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());
    EXPECT_EQ(bins.download(), cpu_histogram256(data.data(), data.size()));
}

INSTANTIATE_TEST_SUITE_P(Sizes, HistogramTest, ::testing::ValuesIn(kSizes), size_name);

class HistogramEdgeTest : public GpuTest {};

TEST_F(HistogramEdgeTest, AllElementsInOneBinIsTheMaximumContentionCase) {
    const int n = 1 << 20;
    DeviceBuffer<uint8_t> in(std::vector<uint8_t>(n, 7));
    DeviceBuffer<unsigned int> bins(256);
    CUDA_ASSERT_OK(launch_histogram256(in.get(), bins.get(), n, 256, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    const auto h = bins.download();
    EXPECT_EQ(h[7], static_cast<unsigned>(n));
    for (int i = 0; i < 256; ++i) {
        if (i != 7) {
            ASSERT_EQ(h[i], 0u) << "bin " << i;
        }
    }
}

TEST_F(HistogramEdgeTest, WorksWithNonPowerOfTwoAndTinyBlockSizes) {
    const int n = 50000;
    const auto data = random_bytes(n, 16);
    const auto expected = cpu_histogram256(data.data(), data.size());
    DeviceBuffer<uint8_t> in(data);
    DeviceBuffer<unsigned int> bins(256);
    for (int block : {1, 33, 100, 1000}) {
        CUDA_ASSERT_OK(launch_histogram256(in.get(), bins.get(), n, block, nullptr));
        CUDA_ASSERT_OK(cudaDeviceSynchronize());
        EXPECT_EQ(bins.download(), expected) << "block=" << block;
    }
}

TEST_F(HistogramEdgeTest, RelaunchOverwritesRatherThanAccumulates) {
    const auto data = random_bytes(4096, 17);
    DeviceBuffer<uint8_t> in(data);
    DeviceBuffer<unsigned int> bins(256);
    for (int i = 0; i < 3; ++i) {
        CUDA_ASSERT_OK(launch_histogram256(in.get(), bins.get(), 4096, 128, nullptr));
        CUDA_ASSERT_OK(cudaDeviceSynchronize());
        unsigned total = 0;
        for (unsigned c : bins.download()) total += c;
        EXPECT_EQ(total, 4096u) << "launch " << i;
    }
}

TEST_F(HistogramEdgeTest, InvalidBlockSizesAreRejected) {
    DeviceBuffer<uint8_t> in(std::vector<uint8_t>(16, 1));
    DeviceBuffer<unsigned int> bins(256);
    EXPECT_EQ(launch_histogram256(in.get(), bins.get(), 16, 0, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_histogram256(in.get(), bins.get(), 16, 2048, nullptr), cudaErrorInvalidValue);
}

// =================================================================================================
// transpose
// =================================================================================================

struct Dim {
    int rows, cols;
};

class TransposeTest : public GpuFixture<::testing::TestWithParam<Dim>> {};

TEST_P(TransposeTest, MatchesTheCpuTransposeAndIsItsOwnInverse) {
    const auto [rows, cols] = GetParam();
    const auto in = random_floats(static_cast<size_t>(rows) * cols, 18);
    std::vector<float> expected(in.size());
    cpu_transpose(in.data(), expected.data(), rows, cols);

    DeviceBuffer<float> din(in), dout(in.size()), dback(in.size());
    CUDA_ASSERT_OK(launch_transpose(din.get(), dout.get(), rows, cols, nullptr));
    CUDA_ASSERT_OK(launch_transpose(dout.get(), dback.get(), cols, rows, nullptr));
    CUDA_ASSERT_OK(cudaDeviceSynchronize());

    const auto r1 = allclose(dout.download(), expected, 0, 0);
    EXPECT_TRUE(r1.ok) << "forward: " << r1.message;
    const auto r2 = allclose(dback.download(), in, 0, 0);
    EXPECT_TRUE(r2.ok) << "round trip: " << r2.message;
}

INSTANTIATE_TEST_SUITE_P(Shapes, TransposeTest,
                         ::testing::Values(Dim{1, 1}, Dim{1, 100}, Dim{100, 1}, Dim{31, 33}, Dim{32, 32}, Dim{64, 128},
                                           Dim{257, 131}, Dim{1000, 3}),
                         [](const ::testing::TestParamInfo<Dim>& i) {
                             return "R" + std::to_string(i.param.rows) + "_C" + std::to_string(i.param.cols);
                         });

TEST(TransposeArgs, NullPointersAndNegativeDimensionsAreRejectedWithoutAGpu) {
    // Argument validation happens before any CUDA call, so this check runs even with no device.
    float x = 0;
    EXPECT_EQ(launch_transpose(nullptr, &x, 2, 2, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_transpose(&x, &x, -1, 2, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_transpose(nullptr, nullptr, 0, 5, nullptr), cudaSuccess);  // empty: no-op
}

TEST(LauncherArgs, ValidationRunsBeforeAnyCudaCallSoItWorksWithoutAGpu) {
    float f = 0;
    uint32_t u = 0;
    unsigned long long ull = 0;
    unsigned int bins = 0;
    uint8_t byte = 0;
    EXPECT_EQ(launch_vector_add(&f, &f, &f, -5, 256, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_reduce_sum(&u, &ull, 4, 33, nullptr), cudaErrorInvalidValue);    // not a power of two
    EXPECT_EQ(launch_reduce_sum(&u, nullptr, 4, 256, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_gemm_naive(&f, &f, nullptr, 2, 2, 2, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_histogram256(&byte, &bins, 4, 0, nullptr), cudaErrorInvalidValue);
    EXPECT_EQ(launch_histogram256(&byte, nullptr, 4, 128, nullptr), cudaErrorInvalidValue);
}
