#pragma once
#include <chrono>
#include <cstdint>

namespace AstraLib {
    namespace Time {
        inline int64_t unixTimestampNS() {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()
            ).count();
        }

        inline int64_t unixTimestampMS() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
            ).count();
        }

} // namespace Time
} // namespace AstraLib