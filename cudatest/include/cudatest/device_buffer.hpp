#pragma once

#include <cuda_runtime_api.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cudatest {

inline void cuda_check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        throw std::runtime_error(std::string(what) + ": " + cudaGetErrorName(e) + " (" + cudaGetErrorString(e) + ")");
    }
}

// Owning, move-only device allocation of `n` elements of T. Throws on any CUDA error so tests
// fail with a readable message instead of continuing with a bad pointer.
template <class T>
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(size_t n) : n_(n) {
        if (n_) cuda_check(cudaMalloc(reinterpret_cast<void**>(&p_), n_ * sizeof(T)), "cudaMalloc");
    }
    explicit DeviceBuffer(const std::vector<T>& host) : DeviceBuffer(host.size()) { upload(host); }
    ~DeviceBuffer() { reset(); }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    DeviceBuffer(DeviceBuffer&& o) noexcept : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
    DeviceBuffer& operator=(DeviceBuffer&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = std::exchange(o.p_, nullptr);
            n_ = std::exchange(o.n_, 0);
        }
        return *this;
    }

    T* get() { return p_; }
    const T* get() const { return p_; }
    size_t size() const { return n_; }

    void upload(const std::vector<T>& host) {
        if (host.size() > n_) throw std::runtime_error("upload larger than allocation");
        if (!host.empty()) cuda_check(cudaMemcpy(p_, host.data(), host.size() * sizeof(T), cudaMemcpyHostToDevice), "H2D");
    }
    std::vector<T> download() const {
        std::vector<T> host(n_);
        if (n_) cuda_check(cudaMemcpy(host.data(), p_, n_ * sizeof(T), cudaMemcpyDeviceToHost), "D2H");
        return host;
    }
    void fill_bytes(int value) {
        if (n_) cuda_check(cudaMemset(p_, value, n_ * sizeof(T)), "cudaMemset");
    }

private:
    void reset() {
        if (p_) cudaFree(p_);  // never throw from a destructor
        p_ = nullptr;
        n_ = 0;
    }
    T* p_ = nullptr;
    size_t n_ = 0;
};

}  // namespace cudatest
