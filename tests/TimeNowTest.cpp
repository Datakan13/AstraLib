// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Time::unixTimestampNS / unixTimestampMS.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Time/timeNow.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

static long g_failures = 0;

#define CHECK_CTX(cond, ctx)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__            \
                      << "  CHECK(" #cond ")  " << ctx << "\n";              \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_CTX(cond, "")

using namespace AstraLib::Time;

// Sane bounds: 2020-01-01 .. 2100-01-01, in nanoseconds since epoch.
static constexpr int64_t YEAR_2020_NS = 1577836800LL * 1'000'000'000LL;
static constexpr int64_t YEAR_2100_NS = 4102444800LL * 1'000'000'000LL;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Values land in a sane calendar range (catches unit mix-ups: seconds
//    passed off as ms, ms passed off as ns, etc.)
// ─────────────────────────────────────────────────────────────────────────────
static void test_sane_range() {
    int64_t ns = unixTimestampNS();
    CHECK_CTX(ns > YEAR_2020_NS && ns < YEAR_2100_NS,
              "unixTimestampNS() = " << ns << " is not between 2020 and 2100");

    int64_t ms = unixTimestampMS();
    CHECK_CTX(ms > YEAR_2020_NS / 1'000'000 && ms < YEAR_2100_NS / 1'000'000,
              "unixTimestampMS() = " << ms << " is not between 2020 and 2100");
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Agrees with std::chrono::system_clock::now(), taken immediately before
//    and after, within a generous tolerance (scheduling jitter between the
//    two calls, not a precision claim about the library itself).
// ─────────────────────────────────────────────────────────────────────────────
static void test_agrees_with_system_clock() {
    using namespace std::chrono;
    int64_t before = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    int64_t ns = unixTimestampNS();
    int64_t after = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();

    CHECK_CTX(ns >= before && ns <= after,
              "unixTimestampNS() = " << ns << " not bracketed by system_clock readings ["
              << before << ", " << after << "]");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. NS and MS agree with each other (called back-to-back, so allow a small
//    tolerance for the gap between the two calls).
// ─────────────────────────────────────────────────────────────────────────────
static void test_ns_and_ms_agree() {
    int64_t ns = unixTimestampNS();
    int64_t ms = unixTimestampMS();
    int64_t nsAsMs = ns / 1'000'000;
    int64_t diff = nsAsMs > ms ? nsAsMs - ms : ms - nsAsMs;
    CHECK_CTX(diff <= 50, "unixTimestampNS()/1e6 = " << nsAsMs
              << " vs unixTimestampMS() = " << ms << " — differ by " << diff << " ms");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Non-decreasing across a tight sequential loop (system_clock isn't
//    guaranteed monotonic across NTP steps, but within a microseconds-long
//    test loop it must never go backwards).
// ─────────────────────────────────────────────────────────────────────────────
static void test_non_decreasing_sequential() {
    int64_t prev = unixTimestampNS();
    for (int i = 0; i < 100000; ++i) {
        int64_t now = unixTimestampNS();
        CHECK_CTX(now >= prev, "timestamp went backwards: " << prev << " -> " << now << " at iteration " << i);
        prev = now;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Thread safety: many threads calling it concurrently must all get sane,
//    non-garbage values (the function wraps stateless chrono calls, so this
//    mainly guards against a future refactor introducing shared state).
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_calls_stay_sane() {
    constexpr int THREADS = 16;
    std::atomic<long> badValues{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                int64_t ns = unixTimestampNS();
                if (!(ns > YEAR_2020_NS && ns < YEAR_2100_NS))
                    badValues.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (auto& th : ts) th.join();
    CHECK_CTX(badValues.load() == 0, badValues.load() << " out-of-range timestamp(s) under concurrent calls");
}

int main() {
    std::cout << "timeNow test suite\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"sane_range",                  test_sane_range},
        {"agrees_with_system_clock",    test_agrees_with_system_clock},
        {"ns_and_ms_agree",             test_ns_and_ms_agree},
        {"non_decreasing_sequential",   test_non_decreasing_sequential},
        {"concurrent_calls_stay_sane",  test_concurrent_calls_stay_sane},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        long before = g_failures;
        t.fn();
        std::cout << (g_failures == before ? "[  OK  ] " : "[ BAD  ] ") << t.name << std::endl;
    }

    if (g_failures == 0) { std::cout << "\nALL TIMENOW TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
