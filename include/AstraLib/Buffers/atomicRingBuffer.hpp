#pragma once
#include <atomic>
#include <immintrin.h>
#include <memory>
#include <AstraLib/Atomic/paddedAtomic.hpp>
#include <type_traits>
namespace AstraLib {
namespace Buffers {

    
template<typename T>
// seq == ticket write 
// seq == ticket + 1 read
struct alignas(64) Slot {
    static_assert(sizeof(T) <= 64, "Cache line overflow: A too large for ring buffer slot");
    std::atomic<int64_t> seq{0};
    T data;
};

template<typename A,std::size_t SIZE,std::size_t BATCH_SIZE = 50>
class AtomicRingBuffer {
    static_assert(SIZE && !(SIZE & (SIZE - 1)),"Size must be a power of two");
    static_assert(SIZE >= 2,"Size must be at least 2");
    std::array<A,BATCH_SIZE> batchDequeueArray;
    Slot<A> buffer[SIZE];
    AstraLib::Atomic::PaddedAtomic<int64_t> writeTicket{0};
    AstraLib::Atomic::PaddedAtomic<int64_t> readTicket{0};
    private:
    void prepareBuffer(){
        for(int i = 0; i < SIZE;i++) {
            buffer[i].seq.store(i,std::memory_order_relaxed);
        }
    }
    public:
    void enqueue(A&& data) {
        uint64_t ticket = writeTicket.value.fetch_add(1,std::memory_order_acq_rel);
        int index = ticket & (SIZE - 1); 
        while(buffer[index].seq.load(std::memory_order_acquire) != ticket) {
            _mm_pause();
        }
        buffer[index].data = std::move(data);
        buffer[index].seq.store(ticket+1,std::memory_order_release);
    }

    void enqueue(const A& data){
        uint64_t ticket = writeTicket.value.fetch_add(1,std::memory_order_acq_rel);
        int index = ticket & (SIZE - 1); 
        while(buffer[index].seq.load(std::memory_order_acquire) != ticket) {
            _mm_pause();
        }
        buffer[index].data = data;
        buffer[index].seq.store(ticket+1,std::memory_order_release);
    }

    A dequeue() {
        uint64_t ticket = readTicket.value.fetch_add(1,std::memory_order_acq_rel);
        int index = ticket & (SIZE - 1);
        while(buffer[index].seq.load(std::memory_order_acquire) != ticket + 1) {
            _mm_pause();
        }
        A data = std::move(buffer[index].data);
        // SIZE is capacity thus +SIZE for the next iteration
        buffer[index].seq.store(ticket+SIZE,std::memory_order_release);
        return data;
    }
    
    void clearBuffer(){
        writeTicket.value.store(0,std::memory_order_release);
        readTicket.value.store(0,std::memory_order_release);
        //Overwrites all the seq values so it's at the starting position again
        prepareBuffer();
    }
    
    // Takes all the fields of the given struct and directly writes them 
    template<typename... Args>
    void emplaceEnqueue(Args&&... args) {
        uint64_t ticket = writeTicket.value.fetch_add(1,std::memory_order_acq_rel);
        int index = ticket & (SIZE - 1); 
        while(buffer[index].seq.load(std::memory_order_acquire) != ticket) {
            _mm_pause();
        }

        if constexpr (std::is_trivially_destructible_v<A>){
            new (&buffer[index].data) A(std::forward<Args>(args)...);
        } else {
            buffer[index].data.~A();
            new (&buffer[index].data) A(std::forward<Args>(args)...);
        }
        buffer[index].seq.store(ticket+1,std::memory_order_release);
    }

    // WARNING: only for single-threaded initialization before any enqueues/dequeues.
    // This bypasses ticket counters and synchronization logic.
    // Use strictly to preconfigure slot memory (e.g., wiring pointers).
    // Does not guarantee atomicity or visibility across threads.
    A* claimPtrNoSync(const int index){
        A* ptr = &buffer[index].data;
        return ptr;
    }

    //WARNING: only use in MPSC paths
    // A max of 50 can be dequeued at a time for defauly construction.
    std::array<A,BATCH_SIZE>* batchDequeue(const int count) {
        for(int i = 0; i< count; i++) {
            uint64_t ticket = readTicket.value.fetch_add(1,std::memory_order_acq_rel);
            int index = ticket & (SIZE - 1);
            while(buffer[index].seq.load(std::memory_order_acquire) != ticket + 1) {
                _mm_pause();
            }
            batchDequeueArray[i] = std::move(buffer[index].data);
            // SIZE is capacity thus +SIZE for the next iteration
            buffer[index].seq.store(ticket+SIZE,std::memory_order_release);
        }
        return &batchDequeueArray;
    }

    public:
    AtomicRingBuffer() {
        prepareBuffer();
    }
};

} // namespace Buffers
} // namespace AstraLib
