#pragma once
#include <iostream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <atomic>
namespace AstraLib {
namespace Debug {


inline std::atomic<bool> writing{false};
    
template<typename... Args> 
inline void debugMessage(Args&&... args) {
    using namespace std::chrono;
    // Get current time
    auto now = system_clock::now();
    auto now_time_t = system_clock::to_time_t(now);
    auto now_us = duration_cast<microseconds>(now.time_since_epoch()) % 1'000'000;
    bool expected = false;
    while (!writing.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        expected = false;
        std::this_thread::yield();
    }
    // Format time: [HH:MM:SS.mmmuuu]
    std::tm tm{};
    localtime_r(&now_time_t, &tm);
    pid_t tid = syscall(SYS_gettid); // This matches SPID

    std::cout << "ThreadID (TID): " << tid;
    std::cout << "["
              << std::put_time(&tm, "%H:%M:%S")
              << "." << std::setw(6) << std::setfill('0') << now_us.count()
              << "] [Debug] ";
    (std::cout << ... << args);
    std::cout << std::endl;
    writing.store(false, std::memory_order_release);
}

} // namespace Debug
} // namespace AstraLib
