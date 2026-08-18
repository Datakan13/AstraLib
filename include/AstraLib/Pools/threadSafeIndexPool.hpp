#pragma once
#include <vector>
#include <array>
#include <AstraLib/Buffers/atomicRingBuffer.hpp>

namespace AstraLib {
namespace Pools {
    


template<std::size_t SIZE>
class ThreadSafeIndexPool {

    alignas(64) AstraLib::Buffers::AtomicRingBuffer<int,SIZE> IndexPool;
    
    void initializeIndexes(){
        for(int i = 0; i<SIZE;i++){
            IndexPool.enqueue(std::move(i));
        }
    }
    public:
    int getIndex(){
        int out =IndexPool.dequeue();
        return out;
    }

    void returnIndex(int in){
        IndexPool.enqueue(std::move(in));
    }

    // Clears internal buffer and sets fresh indexes 
    void reInitializePool(){
        IndexPool.clearBuffer();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        initializeIndexes();
    }

    ThreadSafeIndexPool(){
        initializeIndexes();
    }
};

} // namesapce Pools
} // namespace AstraLib