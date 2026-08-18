#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <chrono>
#include <immintrin.h>
#include <AstraLib/Atomic/paddedAtomic.hpp>

namespace AstraLib {
namespace Atomic{

class alignas(64) Spinlock{
    PaddedAtomic<bool> atomic;
    int64_t waitTime = 1000;

    public:
    void lock() {
        while (atomic.value.exchange(true, std::memory_order_acquire)) {
            for (int i = 0; i < waitTime; ++i) {
                _mm_pause();
            }
        }
    }

    void unlock(){
        atomic.value.store(false, std::memory_order_release);
    }

    Spinlock() {
        atomic.value.store(false,std::memory_order_release);
    }
    Spinlock(int64_t waitTime_) : waitTime(waitTime_){
        atomic.value.store(false,std::memory_order_release);
    }
};

class alignas(64) SpinlockGuard{
    Spinlock& lock;
    public:
    explicit SpinlockGuard(Spinlock& l) : lock(l) { lock.lock(); }
    ~SpinlockGuard() { lock.unlock(); }
    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;
};

} // namespace Atomic
} // namespace AstraLib