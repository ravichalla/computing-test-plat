// Host-callable launchers for the kernels under test. The declarations are plain C++ so test
// code can be built with the ordinary host compiler; only kernels.cu needs nvcc.
//
// Every launcher validates its arguments, launches, and returns cudaGetLastError(), so a bad
// launch configuration is reported to the caller instead of poisoning later calls.
#pragma once

#include <cuda_runtime_api.h>

#include <cstdint>

namespace cudatest {

// c[i] = a[i] + b[i]. block in [1, 1024]. n == 0 is a successful no-op.
cudaError_t launch_vector_add(const float* a, const float* b, float* c, int n, int block, cudaStream_t stream);

// *out = sum(in[0..n)) accumulated in 64 bits. `out` is zeroed by the launcher.
// block must be a power of two in [32, 1024].
cudaError_t launch_reduce_sum(const uint32_t* in, unsigned long long* out, int n, int block, cudaStream_t stream);

// Row-major C[M x N] = A[M x K] * B[K x N]. Naive one-thread-per-output and shared-memory tiled
// versions; they must agree with each other and with the CPU reference.
cudaError_t launch_gemm_naive(const float* A, const float* B, float* C, int M, int N, int K, cudaStream_t stream);
cudaError_t launch_gemm_tiled(const float* A, const float* B, float* C, int M, int N, int K, cudaStream_t stream);

// bins[b] = number of bytes equal to b. `bins` (256 entries) is zeroed by the launcher.
cudaError_t launch_histogram256(const uint8_t* in, unsigned int* bins, int n, int block, cudaStream_t stream);

// out[c * rows + r] = in[r * cols + c].
cudaError_t launch_transpose(const float* in, float* out, int rows, int cols, cudaStream_t stream);

}  // namespace cudatest
