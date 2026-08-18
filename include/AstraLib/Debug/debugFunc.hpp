#pragma once
#include <iostream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

namespace AstraLib {
namespace Debug {

    

template<typename... Args> 
inline void debugMessage(Args&&... args) {
    using namespace std::chrono;
    // Get current time
    auto now = system_clock::now();
    auto now_time_t = system_clock::to_time_t(now);
    auto now_us = duration_cast<microseconds>(now.time_since_epoch()) % 1'000'000;

    // Format time: [HH:MM:SS.mmmuuu]
    std::tm tm = *std::localtime(&now_time_t);
    pid_t tid = syscall(SYS_gettid); // This matches SPID
    std::cout << "ThreadID (TID): " << tid;
    std::cout << "["
              << std::put_time(&tm, "%H:%M:%S")
              << "." << std::setw(6) << std::setfill('0') << now_us.count()
              << "] [Debug] ";
    (std::cout << ... << args);
    std::cout << std::endl;
}

} // namespace Debug
} // namespace AstraLib
