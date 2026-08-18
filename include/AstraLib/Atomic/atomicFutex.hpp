#pragma once 
#include <linux/futex.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>
#include <atomic> 
#include <thread>

namespace AstraLib{
namespace Atomic {

class alignas(64) AtomicFutex {
    private:
    std::atomic<int> futex_val;

    public:
    void wait() {
        int expected = futex_val.load(std::memory_order_seq_cst);

        while (true) {
            int res = syscall(SYS_futex, &futex_val, FUTEX_WAIT, expected, nullptr, nullptr, 0);
            if (res == -1 && errno != EAGAIN && errno != EINTR) {
                return;
            }
            int current = futex_val.load(std::memory_order_acquire);
            if (current != expected) break; 
        }
    }

    // Will wake up 
    void wake() {
        futex_val.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        syscall(SYS_futex, &futex_val, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }
    
    AtomicFutex() {
        futex_val.store(0,std::memory_order_release);
    }

};
   } // namespace Atomic
} // namespace AstraLib