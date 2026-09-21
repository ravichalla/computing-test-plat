#include <algorithm>

#include "cudatest/kernels.hpp"

namespace cudatest {

namespace {

constexpr int kMaxThreadsPerBlock = 1024;
constexpr int kTile = 16;         // GEMM tile edge
constexpr int kTransTile = 32;    // transpose tile edge
constexpr int kTransRows = 8;     // rows handled per pass by a transpose block

bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

// Number of blocks for a grid-stride loop: enough to cover n, but never more than needed.
int blocks_for(int n, int block) { return std::max(1, std::min((n + block - 1) / block, 65535)); }

// ---------------------------------------------------------------------------------------------
// vector add
// ---------------------------------------------------------------------------------------------
__global__ void vector_add_kernel(const float* a, const float* b, float* c, int n) {
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) c[i] = a[i] + b[i];
}

// ---------------------------------------------------------------------------------------------
// reduction: per-block tree in shared memory, then one 64-bit atomic per block
// ---------------------------------------------------------------------------------------------
__global__ void reduce_sum_kernel(const uint32_t* in, unsigned long long* out, int n) {
    extern __shared__ unsigned long long partial[];

    unsigned long long v = 0;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) v += in[i];
    partial[threadIdx.x] = v;
    __syncthreads();

    for (unsigned s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s) partial[threadIdx.x] += partial[threadIdx.x + s];
        __syncthreads();
    }
    if (threadIdx.x == 0) atomicAdd(out, partial[0]);
}

// ---------------------------------------------------------------------------------------------
// GEMM
// ---------------------------------------------------------------------------------------------
__global__ void gemm_naive_kernel(const float* A, const float* B, float* C, int M, int N, int K) {
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    const int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= M || col >= N) return;
    float acc = 0.0f;
    for (int k = 0; k < K; ++k) acc += A[row * K + k] * B[k * N + col];
    C[row * N + col] = acc;
}

__global__ void gemm_tiled_kernel(const float* A, const float* B, float* C, int M, int N, int K) {
    __shared__ float As[kTile][kTile];
    __shared__ float Bs[kTile][kTile];

    const int row = blockIdx.y * kTile + threadIdx.y;
    const int col = blockIdx.x * kTile + threadIdx.x;
    float acc = 0.0f;

    for (int t = 0; t < (K + kTile - 1) / kTile; ++t) {
        const int ak = t * kTile + threadIdx.x;  // column of A this thread loads
        const int bk = t * kTile + threadIdx.y;  // row of B this thread loads
        As[threadIdx.y][threadIdx.x] = (row < M && ak < K) ? A[row * K + ak] : 0.0f;
        Bs[threadIdx.y][threadIdx.x] = (bk < K && col < N) ? B[bk * N + col] : 0.0f;
        __syncthreads();
        for (int k = 0; k < kTile; ++k) acc += As[threadIdx.y][k] * Bs[k][threadIdx.x];
        __syncthreads();
    }
    if (row < M && col < N) C[row * N + col] = acc;
}

// ---------------------------------------------------------------------------------------------
// histogram: privatised in shared memory to keep global atomics to one per bin per block
// ---------------------------------------------------------------------------------------------
__global__ void histogram256_kernel(const uint8_t* in, unsigned int* bins, int n) {
    __shared__ unsigned int local[256];
    for (int i = threadIdx.x; i < 256; i += blockDim.x) local[i] = 0;
    __syncthreads();

    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) atomicAdd(&local[in[i]], 1u);
    __syncthreads();

    for (int i = threadIdx.x; i < 256; i += blockDim.x) {
        if (local[i]) atomicAdd(&bins[i], local[i]);
    }
}

// ---------------------------------------------------------------------------------------------
// transpose: shared-memory tile padded by one column to avoid bank conflicts
// ---------------------------------------------------------------------------------------------
__global__ void transpose_kernel(const float* in, float* out, int rows, int cols) {
    __shared__ float tile[kTransTile][kTransTile + 1];

    int x = blockIdx.x * kTransTile + threadIdx.x;  // input column
    int y = blockIdx.y * kTransTile + threadIdx.y;  // input row (first of kTransTile/kTransRows passes)
    for (int j = 0; j < kTransTile; j += kTransRows) {
        if (x < cols && (y + j) < rows) tile[threadIdx.y + j][threadIdx.x] = in[(y + j) * cols + x];
    }
    __syncthreads();

    x = blockIdx.y * kTransTile + threadIdx.x;  // output column == input row
    y = blockIdx.x * kTransTile + threadIdx.y;  // output row    == input column
    for (int j = 0; j < kTransTile; j += kTransRows) {
        if (x < rows && (y + j) < cols) out[(y + j) * rows + x] = tile[threadIdx.x][threadIdx.y + j];
    }
}

bool bad_gemm_args(const float* A, const float* B, const float* C, int M, int N, int K) {
    return !A || !B || !C || M < 0 || N < 0 || K < 0;
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// launchers
// -------------------------------------------------------------------------------------------------

cudaError_t launch_vector_add(const float* a, const float* b, float* c, int n, int block, cudaStream_t stream) {
    if (n < 0 || (n > 0 && (!a || !b || !c))) return cudaErrorInvalidValue;
    if (n == 0) return cudaSuccess;
    // block <= 0 or > 1024 is passed through so the driver reports cudaErrorInvalidConfiguration.
    const int grid = block > 0 ? blocks_for(n, block) : 1;
    vector_add_kernel<<<grid, block, 0, stream>>>(a, b, c, n);
    return cudaGetLastError();
}

cudaError_t launch_reduce_sum(const uint32_t* in, unsigned long long* out, int n, int block, cudaStream_t stream) {
    if (n < 0 || !out || (n > 0 && !in)) return cudaErrorInvalidValue;
    if (!is_pow2(block) || block < 32 || block > kMaxThreadsPerBlock) return cudaErrorInvalidValue;
    cudaError_t e = cudaMemsetAsync(out, 0, sizeof(*out), stream);
    if (e != cudaSuccess) return e;
    if (n == 0) return cudaSuccess;
    reduce_sum_kernel<<<blocks_for(n, block), block, block * sizeof(unsigned long long), stream>>>(in, out, n);
    return cudaGetLastError();
}

cudaError_t launch_gemm_naive(const float* A, const float* B, float* C, int M, int N, int K, cudaStream_t stream) {
    if (bad_gemm_args(A, B, C, M, N, K)) return cudaErrorInvalidValue;
    if (M == 0 || N == 0) return cudaSuccess;
    const dim3 block(kTile, kTile);
    const dim3 grid((N + kTile - 1) / kTile, (M + kTile - 1) / kTile);
    gemm_naive_kernel<<<grid, block, 0, stream>>>(A, B, C, M, N, K);
    return cudaGetLastError();
}

cudaError_t launch_gemm_tiled(const float* A, const float* B, float* C, int M, int N, int K, cudaStream_t stream) {
    if (bad_gemm_args(A, B, C, M, N, K)) return cudaErrorInvalidValue;
    if (M == 0 || N == 0) return cudaSuccess;
    const dim3 block(kTile, kTile);
    const dim3 grid((N + kTile - 1) / kTile, (M + kTile - 1) / kTile);
    gemm_tiled_kernel<<<grid, block, 0, stream>>>(A, B, C, M, N, K);
    return cudaGetLastError();
}

cudaError_t launch_histogram256(const uint8_t* in, unsigned int* bins, int n, int block, cudaStream_t stream) {
    if (n < 0 || !bins || (n > 0 && !in)) return cudaErrorInvalidValue;
    if (block < 1 || block > kMaxThreadsPerBlock) return cudaErrorInvalidValue;
    cudaError_t e = cudaMemsetAsync(bins, 0, 256 * sizeof(unsigned int), stream);
    if (e != cudaSuccess) return e;
    if (n == 0) return cudaSuccess;
    histogram256_kernel<<<blocks_for(n, block), block, 0, stream>>>(in, bins, n);
    return cudaGetLastError();
}

cudaError_t launch_transpose(const float* in, float* out, int rows, int cols, cudaStream_t stream) {
    if (rows < 0 || cols < 0 || (rows > 0 && cols > 0 && (!in || !out))) return cudaErrorInvalidValue;
    if (rows == 0 || cols == 0) return cudaSuccess;
    const dim3 block(kTransTile, kTransRows);
    const dim3 grid((cols + kTransTile - 1) / kTransTile, (rows + kTransTile - 1) / kTransTile);
    transpose_kernel<<<grid, block, 0, stream>>>(in, out, rows, cols);
    return cudaGetLastError();
}

}  // namespace cudatest
