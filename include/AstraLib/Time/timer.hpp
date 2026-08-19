#pragma once
#include <x86intrin.h>
#include <cpuid.h>
#include <string>
#include <iostream>
#include <x86intrin.h>
#include <unistd.h>       // for sleep
#include <iostream>
#include <string>
#include <cstdint>

namespace AstraLib {
namespace Time {

class Timer {
private:
    unsigned aux;
    unsigned eax, ebx, ecx, edx;
    uint64_t startTime;
    double ghz;

    void setGHz() {
        uint64_t t1 = __rdtscp(&aux);
        sleep(1); // 1 second sleep
        uint64_t t2 = __rdtscp(&aux);
        ghz = (t2 - t1) / 1e9; // cycles per ns = GHz
    }

public:
    Timer() {
        setGHz();  // auto-calibrate once at construction
    }

    void start() {
        __cpuid(0, eax, ebx, ecx, edx);
        startTime = __rdtscp(&aux);
    }

    // Returns the number of cycles elapsed since start() was called
    inline uint64_t getTimeCycles(){
        uint64_t endTime = __rdtscp(&aux);
        __cpuid(0, eax, ebx, ecx, edx);
        uint64_t cycles = endTime - startTime;
        return cycles;
    }

    // Returns the number of nanoseconds elapsed since start() was called
    inline double getTimeNs(){
        uint64_t cycles = getTimeCycles();
        double ns = cycles / ghz;       // convert cycles → nanoseconds
        return ns;
    }

    void write(const std::string& label = "Timer") {
        uint64_t cycles = getTimeCycles();
        double ns = cycles / ghz;       // convert cycles → nanoseconds
        double us = ns / 1000.0;        // also microseconds

        std::cout << "[" << label << "] "
                  << cycles << " cycles, ~"
                  << ns << " ns (~"
                  << us << " µs)"
                  << std::endl;
    }

    
};

} // namespace Time
} // namespace AstraLib