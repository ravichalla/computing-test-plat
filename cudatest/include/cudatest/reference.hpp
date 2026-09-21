// Host-only reference implementations and comparison helpers.
//
// Nothing here needs CUDA. That is deliberate: the "oracle" the GPU results are checked
// against is itself unit tested on any machine, so a failing GPU test points at the GPU
// code and not at a broken expectation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cudatest {

// ---- deterministic test data (xorshift64*) -----------------------------------------------

class Rng {
public:
    explicit Rng(uint64_t seed);
    uint64_t next();
    float uniform(float lo, float hi);  // in [lo, hi)
    uint32_t below(uint32_t n);         // in [0, n)

private:
    uint64_t s_;
};

std::vector<float> random_floats(size_t n, uint64_t seed, float lo = -1.0f, float hi = 1.0f);
std::vector<uint32_t> random_u32(size_t n, uint64_t seed, uint32_t max_exclusive);
std::vector<uint8_t> random_bytes(size_t n, uint64_t seed);

// ---- references --------------------------------------------------------------------------

void cpu_vector_add(const float* a, const float* b, float* c, size_t n);
uint64_t cpu_reduce_sum(const uint32_t* in, size_t n);
// Row-major C[M x N] = A[M x K] * B[K x N], accumulated in double so it is a good oracle
// for single-precision GPU kernels.
void cpu_gemm(const float* A, const float* B, float* C, int M, int N, int K);
std::vector<uint32_t> cpu_histogram256(const uint8_t* in, size_t n);
void cpu_transpose(const float* in, float* out, int rows, int cols);

// ---- comparison --------------------------------------------------------------------------

struct CompareResult {
    bool ok = true;
    size_t first_bad = 0;
    double max_abs_err = 0;
    double max_rel_err = 0;
    std::string message;  // empty when ok; otherwise describes the first mismatch
};

// |a - b| <= atol + rtol * |b| element-wise. NaN never matches (unless equal_nan and both are
// NaN); infinities match only the same infinity. rtol = atol = 0 demands bit-exact equality
// for finite values.
CompareResult allclose(const float* actual, const float* expected, size_t n, double rtol, double atol,
                       bool equal_nan = false);

inline CompareResult allclose(const std::vector<float>& actual, const std::vector<float>& expected, double rtol,
                              double atol, bool equal_nan = false) {
    if (actual.size() != expected.size()) {
        CompareResult r;
        r.ok = false;
        r.message = "size mismatch: " + std::to_string(actual.size()) + " vs " + std::to_string(expected.size());
        return r;
    }
    return allclose(actual.data(), expected.data(), actual.size(), rtol, atol, equal_nan);
}

}  // namespace cudatest
