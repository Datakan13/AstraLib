#pragma once
#include <thread>
#include <functional>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <queue>
#include <AstraLib/Buffers/atomicRingBuffer.hpp>

namespace AstraLib {
namespace Threading {

constexpr size_t threadCount = 4;

void set_low_priority(std::thread &t) {
    sched_param sch_params;
    sch_params.sched_priority = 0; // lowest for SCHED_OTHER

    // Default Linux threads use SCHED_OTHER, which ignores sched_priority.
    // To make priority changes effective, you’d need SCHED_BATCH or SCHED_IDLE.
    pthread_setschedparam(t.native_handle(), SCHED_IDLE, &sch_params);
}


// ─────────────────────────────────────────────
// Lightweight futex-based gate for thread sleep/wake
// ─────────────────────────────────────────────
class ThreadGate {
    std::atomic<int>& flag;

public:
    explicit ThreadGate(std::atomic<int>& flag_) : flag(flag_) {}

    void waiter() {
        while (true) {
            int expected = 0;
            if (flag.load(std::memory_order_acquire) == 1) break;

            syscall(SYS_futex, &flag, FUTEX_WAIT, expected, nullptr, nullptr, 0);
        }
    }

    void signaler() {
        flag.store(1, std::memory_order_release);
        syscall(SYS_futex, &flag, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    void reset() {
        flag.store(0, std::memory_order_release);
    }
};

// ─────────────────────────────────────────────
// Worker thread object with a task and wake mechanism
// ─────────────────────────────────────────────
class Worker {
    std::atomic<int> flag;
    std::thread thread;
    AstraLib::Threading::ThreadGate gate;
    std::function<void()> task;

public:
    std::atomic<int> poolFlag;  // External flag visible to ThreadPool

private:
    bool running;

    void thread_loop() {
        while (running) {
            gate.waiter();
            std::atomic_thread_fence(std::memory_order_acquire);
            
            if (task) {
                task(); // Execute assigned task
                task = nullptr;

                // Mark worker as available again
                poolFlag.store(0, std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                gate.reset();  // Reset internal gate
            }
        }
    }

public:
    void activateWorker(std::function<void()> task_) {
        task = std::move(task_);
        std::atomic_thread_fence(std::memory_order_release);
        gate.signaler();
    }

    Worker() : flag(0), gate(flag), running(true) {
        poolFlag.store(0,std::memory_order_release);
        thread = std::thread([this] { this->thread_loop(); });
    }

    ~Worker() {
        running = false;
        gate.signaler(); // Wake to allow exit
        if (thread.joinable())
            thread.join();
    }
};

// ─────────────────────────────────────────────
// ThreadPool handles task dispatching to workers
// ─────────────────────────────────────────────
class ThreadPool {
    std::atomic<int> taskFlag;
    std::thread dispatcherThread;
    std::array<Worker, threadCount> threads;
    std::array<std::atomic<int>*, threadCount> threadFlags;
    AstraLib::Buffers::AtomicRingBuffer<std::function<void()>,1024> taskQueue;
    AstraLib::Threading::ThreadGate taskGate;
    
    bool running;

    void registerWorkers() {
        int id = 0;
        for (Worker& thread : threads) {
            threadFlags[id++] = &thread.poolFlag;
        }
    }

public:
    void assignTask(std::function<void()>&& task) {
        taskQueue.enqueue(std::move(task));
        std::atomic_thread_fence(std::memory_order_release);
        taskGate.signaler();
    }

private:
    int lastIdDispatched = 0;

    void taskDispatcher(int& id) {
        std::function<void()> task;

        while (running) {
            taskGate.waiter();
            std::atomic_thread_fence(std::memory_order_acquire);

            while (true) {
                task = taskQueue.dequeue();
                size_t start = id;
                bool assigned = false;

                // Try round-robin assignment until a free thread is found
                do {
                    std::atomic<int>* flag = threadFlags[id];
                    int expected = 0;
                    if (flag->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                        threads[id].activateWorker(std::move(task));
                        id = (id + 1) % threadCount;
                        assigned = true;
                        break;
                    }
                    id = (id + 1) % threadCount;
                } while (id != start);
                if(!assigned){
                    for(;;) {
                        do {
                            std::atomic<int>* flag = threadFlags[id];
                            int expected = 0;
                            if (flag->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                                threads[id].activateWorker(std::move(task));
                                id = (id + 1) % threadCount;
                                assigned = true;
                                break;
                            }
                            id = (id + 1) % threadCount;
                        } while (id != start);
                        if (assigned) break;
                        _mm_pause();
                    }
                    
                }
                
            }
        }
    }

public:
    ThreadPool() : taskGate(taskFlag) {
        taskFlag.store(0,std::memory_order_release);
        registerWorkers();
        running = true;
        dispatcherThread = std::thread([this] {
            this->taskDispatcher(lastIdDispatched);
        });
    }

    ~ThreadPool() {
        running = false;
        taskGate.signaler();
        if (dispatcherThread.joinable())
            dispatcherThread.join();
    }
};

} // namespace threading
} // namespace AstraLib