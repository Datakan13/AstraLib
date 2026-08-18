#pragma once 
#include <atomic>
#include <cstdint>

namespace AstraLib {
namespace Atomic{


template<typename T = int>
struct alignas(64) PaddedAtomic {
    std::atomic<T> value;
    char padding[64 - sizeof(std::atomic<T>)];

    PaddedAtomic() {}
    PaddedAtomic(T value_) : value(value_) {}
};
static_assert(sizeof(PaddedAtomic<int>) == 64, "PaddedAtomic must be exactly 64 bytes!");

        
} // namespace Atomic
} // namespace AstraLib