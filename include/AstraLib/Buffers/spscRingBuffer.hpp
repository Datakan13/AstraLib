#pragma once
#include <array>
#include <atomic>
#include <AstraLib/Atomic/paddedAtomic.hpp>
#include <immintrin.h>
#include <type_traits>
#include <new>
#include <utility>

namespace AstraLib {
namespace Buffers {


template<typename T>
struct alignas(128) spscSlot{
    public:
    T data;
    // 0 means empty 1 means filled
    std::atomic<int> ticket = 0;
};

template<typename A,std::size_t BUFFER_SIZE>
class spscRingBuffer{
    std::array<spscSlot<A>,BUFFER_SIZE> buffer;
    alignas(128) int lastRead = 0;
    alignas(128) int lastWritten = 0;
    public:
    void enqueue(A&& data){
        if(lastWritten+1 < BUFFER_SIZE){
            lastWritten+=1;
        } else {
            lastWritten = 0;
        }
        while(buffer[lastWritten].ticket.load(std::memory_order_acquire) != 0){
            _mm_pause();
        }
        buffer[lastWritten].data = std::move(data);
        buffer[lastWritten].ticket.store(1,std::memory_order_release);
    }

    template<typename... Args>
    void emplaceEnqueue(Args&&... args){
        if(lastWritten+1 < BUFFER_SIZE){
            lastWritten+=1;
        } else {
            lastWritten = 0;
        }
        while(buffer[lastWritten].ticket.load(std::memory_order_acquire) != 0){
            _mm_pause();
        }
        if constexpr (std::is_trivially_destructible_v<A>){
            new (&buffer[lastWritten].data) A(std::forward<Args>(args)...);
        } else {
            buffer[lastWritten].data.~A();
            new (&buffer[lastWritten].data) A(std::forward<Args>(args)...);
        }
        buffer[lastWritten].ticket.store(1,std::memory_order_release);
    }

    A dequeue(){
        if(lastRead+1 < BUFFER_SIZE){
            lastRead+=1;
        } else {
            lastRead = 0;
        }
        while(buffer[lastRead].ticket.load(std::memory_order_acquire) == 0){
            _mm_pause();
        }
        A data = std::move(buffer[lastRead].data);
        buffer[lastRead].ticket.store(0,std::memory_order_release);
        return data;
    }
};
}
}
