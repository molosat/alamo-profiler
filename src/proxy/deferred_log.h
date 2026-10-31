#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>

// No producer performs I/O. A stalled sink never owns the queue lock.
class DeferredLog {
public:
    void Push(const char* text) noexcept {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || size_ == lines_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::snprintf(lines_[size_++].data(), 1024, "%s", text);
    }
    template<class Sink> void Drain(Sink sink) {
        std::array<std::array<char, 1024>, 32> batch;
        size_t count;
        {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) return;
            count = size_;
            for (size_t i = 0; i < count; ++i) batch[i] = lines_[i];
            size_ = 0;
        }
        for (size_t i = 0; i < count; ++i) sink(batch[i].data());
    }
    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
private:
    std::mutex mutex_;
    std::array<std::array<char, 1024>, 32> lines_{};
    size_t size_ = 0;
    std::atomic<uint64_t> dropped_{0};
};
