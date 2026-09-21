#include "cudatest/reference.hpp"

#include <cmath>
#include <sstream>

namespace cudatest {

Rng::Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

uint64_t Rng::next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
}

float Rng::uniform(float lo, float hi) {
    // Top 24 bits give an exactly representable float in [0, 1).
    const float u = static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
    return lo + u * (hi - lo);
}

uint32_t Rng::below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>((next() >> 32) % n); }

std::vector<float> random_floats(size_t n, uint64_t seed, float lo, float hi) {
    Rng r(seed);
    std::vector<float> v(n);
    for (auto& x : v) x = r.uniform(lo, hi);
    return v;
}

std::vector<uint32_t> random_u32(size_t n, uint64_t seed, uint32_t max_exclusive) {
    Rng r(seed);
    std::vector<uint32_t> v(n);
    for (auto& x : v) x = r.below(max_exclusive);
    return v;
}

std::vector<uint8_t> random_bytes(size_t n, uint64_t seed) {
    Rng r(seed);
    std::vector<uint8_t> v(n);
    for (auto& x : v) x = static_cast<uint8_t>(r.next() >> 56);
    return v;
}

void cpu_vector_add(const float* a, const float* b, float* c, size_t n) {
    for (size_t i = 0; i < n; ++i) c[i] = a[i] + b[i];
}

uint64_t cpu_reduce_sum(const uint32_t* in, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) s += in[i];
    return s;
}

void cpu_gemm(const float* A, const float* B, float* C, int M, int N, int K) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            double acc = 0.0;
            for (int k = 0; k < K; ++k) acc += static_cast<double>(A[i * K + k]) * static_cast<double>(B[k * N + j]);
            C[i * N + j] = static_cast<float>(acc);
        }
    }
}

std::vector<uint32_t> cpu_histogram256(const uint8_t* in, size_t n) {
    std::vector<uint32_t> bins(256, 0);
    for (size_t i = 0; i < n; ++i) ++bins[in[i]];
    return bins;
}

void cpu_transpose(const float* in, float* out, int rows, int cols) {
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) out[c * rows + r] = in[r * cols + c];
    }
}

CompareResult allclose(const float* actual, const float* expected, size_t n, double rtol, double atol,
                       bool equal_nan) {
    CompareResult res;
    for (size_t i = 0; i < n; ++i) {
        const float a = actual[i], e = expected[i];
        bool match;
        double abs_err = 0, rel_err = 0;

        if (std::isnan(a) || std::isnan(e)) {
            match = equal_nan && std::isnan(a) && std::isnan(e);
        } else if (std::isinf(a) || std::isinf(e)) {
            match = (a == e);  // same-signed infinity only
        } else {
            abs_err = std::fabs(static_cast<double>(a) - static_cast<double>(e));
            rel_err = e != 0.0f ? abs_err / std::fabs(static_cast<double>(e)) : 0.0;
            match = abs_err <= atol + rtol * std::fabs(static_cast<double>(e));
        }

        if (abs_err > res.max_abs_err) res.max_abs_err = abs_err;
        if (rel_err > res.max_rel_err) res.max_rel_err = rel_err;

        if (!match && res.ok) {
            res.ok = false;
            res.first_bad = i;
            std::ostringstream os;
            os.precision(9);
            os << "mismatch at index " << i << ": actual=" << a << " expected=" << e << " (rtol=" << rtol
               << ", atol=" << atol << ")";
            res.message = os.str();
        }
    }
    return res;
}

}  // namespace cudatest
