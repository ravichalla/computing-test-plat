// Host-only tests: these run on any machine, with or without a GPU or the CUDA toolkit.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>

#include "cudatest/reference.hpp"

using namespace cudatest;

// ---- test data ----------------------------------------------------------------------------

TEST(Rng, IsDeterministicPerSeedAndDiffersAcrossSeeds) {
    Rng a(42), b(42), c(43);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next(), b.next());
    Rng d(42);
    EXPECT_NE(d.next(), c.next());
}

TEST(Rng, ZeroSeedDoesNotGetStuck) {
    Rng r(0);
    std::set<uint64_t> seen;
    for (int i = 0; i < 100; ++i) seen.insert(r.next());
    EXPECT_GT(seen.size(), 95u);
}

TEST(Rng, UniformStaysInRangeAndCoversIt) {
    Rng r(7);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 20000; ++i) {
        const float x = r.uniform(-2.0f, 3.0f);
        ASSERT_GE(x, -2.0f);
        ASSERT_LT(x, 3.0f);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    EXPECT_LT(lo, -1.9f);
    EXPECT_GT(hi, 2.9f);
}

TEST(Rng, BelowIsBoundedAndHandlesZero) {
    Rng r(9);
    for (int i = 0; i < 5000; ++i) ASSERT_LT(r.below(10), 10u);
    EXPECT_EQ(r.below(0), 0u);
    EXPECT_EQ(r.below(1), 0u);
}

TEST(RandomData, SizesRangesAndReproducibility) {
    const auto f = random_floats(1000, 1, -0.5f, 0.5f);
    ASSERT_EQ(f.size(), 1000u);
    for (float x : f) ASSERT_TRUE(x >= -0.5f && x < 0.5f);
    EXPECT_EQ(f, random_floats(1000, 1, -0.5f, 0.5f));
    EXPECT_NE(f, random_floats(1000, 2, -0.5f, 0.5f));

    for (uint32_t x : random_u32(1000, 3, 17)) ASSERT_LT(x, 17u);
    EXPECT_EQ(random_bytes(0, 1).size(), 0u);

    // bytes should use the whole 0..255 range
    std::set<int> distinct;
    for (uint8_t b : random_bytes(10000, 5)) distinct.insert(b);
    EXPECT_GT(distinct.size(), 250u);
}

// ---- references ---------------------------------------------------------------------------

TEST(Reference, VectorAdd) {
    const float a[] = {1, 2, 3}, b[] = {10, 20, 30};
    float c[3];
    cpu_vector_add(a, b, c, 3);
    EXPECT_EQ(c[0], 11);
    EXPECT_EQ(c[2], 33);
}

TEST(Reference, ReduceSumDoesNotOverflow32Bits) {
    const std::vector<uint32_t> big(1 << 16, 0xFFFFFFFFu);
    EXPECT_EQ(cpu_reduce_sum(big.data(), big.size()), static_cast<uint64_t>(0xFFFFFFFFu) * (1u << 16));
    EXPECT_EQ(cpu_reduce_sum(nullptr, 0), 0u);
}

TEST(Reference, GemmKnownValues) {
    // [1 2; 3 4] * [5 6; 7 8] = [19 22; 43 50]
    const float A[] = {1, 2, 3, 4}, B[] = {5, 6, 7, 8};
    float C[4];
    cpu_gemm(A, B, C, 2, 2, 2);
    EXPECT_EQ(C[0], 19);
    EXPECT_EQ(C[1], 22);
    EXPECT_EQ(C[2], 43);
    EXPECT_EQ(C[3], 50);
}

TEST(Reference, GemmNonSquareShapes) {
    // (1x3) * (3x2)
    const float A[] = {1, 2, 3}, B[] = {1, 0, 0, 1, 1, 1};
    float C[2];
    cpu_gemm(A, B, C, 1, 2, 3);
    EXPECT_EQ(C[0], 1 + 0 + 3);
    EXPECT_EQ(C[1], 0 + 2 + 3);
}

TEST(Reference, GemmWithIdentityReturnsTheInput) {
    const int n = 17;
    const auto A = random_floats(n * n, 11);
    std::vector<float> I(n * n, 0.0f), C(n * n);
    for (int i = 0; i < n; ++i) I[i * n + i] = 1.0f;
    cpu_gemm(A.data(), I.data(), C.data(), n, n, n);
    EXPECT_EQ(C, A);
}

TEST(Reference, Histogram) {
    const uint8_t data[] = {0, 1, 1, 255, 255, 255};
    const auto h = cpu_histogram256(data, sizeof data);
    ASSERT_EQ(h.size(), 256u);
    EXPECT_EQ(h[0], 1u);
    EXPECT_EQ(h[1], 2u);
    EXPECT_EQ(h[255], 3u);
    EXPECT_EQ(h[2], 0u);

    const auto rnd = random_bytes(12345, 3);
    uint64_t total = 0;
    for (uint32_t c : cpu_histogram256(rnd.data(), rnd.size())) total += c;
    EXPECT_EQ(total, 12345u);
}

TEST(Reference, TransposeKnownAndInvolution) {
    const float in[] = {1, 2, 3, 4, 5, 6};  // 2x3
    float out[6];
    cpu_transpose(in, out, 2, 3);
    const float expect[] = {1, 4, 2, 5, 3, 6};  // 3x2
    for (int i = 0; i < 6; ++i) EXPECT_EQ(out[i], expect[i]) << i;

    const auto m = random_floats(31 * 47, 8);
    std::vector<float> t(m.size()), back(m.size());
    cpu_transpose(m.data(), t.data(), 31, 47);
    cpu_transpose(t.data(), back.data(), 47, 31);
    EXPECT_EQ(back, m);
}

// ---- allclose -----------------------------------------------------------------------------

TEST(Allclose, IdenticalArraysMatchExactly) {
    const std::vector<float> a = {1.0f, -2.5f, 0.0f};
    EXPECT_TRUE(allclose(a, a, 0, 0).ok);
}

TEST(Allclose, ToleranceFormulaIsAbsPlusRelTimesExpected) {
    const std::vector<float> expected = {100.0f}, near_ = {100.09f}, far_ = {100.2f};
    EXPECT_TRUE(allclose(near_, expected, 1e-3, 0).ok);   // 0.09 <= 0.1
    EXPECT_FALSE(allclose(far_, expected, 1e-3, 0).ok);   // 0.2  >  0.1
    EXPECT_TRUE(allclose(std::vector<float>{1e-5f}, std::vector<float>{0.0f}, 0, 1e-4).ok);  // atol rescues ~0
}

TEST(Allclose, ReportsTheFirstMismatchAndErrorStatistics) {
    const std::vector<float> e = {1, 2, 3, 4}, a = {1, 2, 3.5f, 9};
    const auto r = allclose(a, e, 0, 0);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.first_bad, 2u);
    EXPECT_DOUBLE_EQ(r.max_abs_err, 5.0);
    EXPECT_NE(r.message.find("index 2"), std::string::npos) << r.message;
}

TEST(Allclose, NaNAndInfinityRules) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(allclose(std::vector<float>{nan}, std::vector<float>{1.0f}, 1, 1).ok);
    EXPECT_FALSE(allclose(std::vector<float>{1.0f}, std::vector<float>{nan}, 1, 1).ok);
    EXPECT_FALSE(allclose(std::vector<float>{nan}, std::vector<float>{nan}, 1, 1).ok);            // NaN != NaN by default
    EXPECT_TRUE(allclose(std::vector<float>{nan}, std::vector<float>{nan}, 0, 0, true).ok);        // unless requested
    EXPECT_TRUE(allclose(std::vector<float>{inf}, std::vector<float>{inf}, 0, 0).ok);
    EXPECT_FALSE(allclose(std::vector<float>{inf}, std::vector<float>{-inf}, 1e9, 1e9).ok);
    EXPECT_FALSE(allclose(std::vector<float>{inf}, std::vector<float>{1e30f}, 1e9, 1e9).ok);
}

TEST(Allclose, SizeMismatchAndEmptyInputs) {
    EXPECT_FALSE(allclose(std::vector<float>{1, 2}, std::vector<float>{1}, 0, 0).ok);
    EXPECT_TRUE(allclose(std::vector<float>{}, std::vector<float>{}, 0, 0).ok);
}
