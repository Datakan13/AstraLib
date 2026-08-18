// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Time::Timer.
//
// Timer::write() only prints (no accessor returns the measured value), so
// the tests here redirect std::cout's streambuf into a std::ostringstream
// for the duration of each write() call and parse the numbers back out of
// the printed line — the only way to assert on this API's output without
// modifying it.
//
// Timer's constructor calibrates GHz with a blocking 1-second sleep, so this
// file builds exactly ONE Timer (in main) and reuses it across every test,
// rather than paying that cost repeatedly.
//
// Bounds are deliberately loose and targets deliberately large: this repo
// runs under WSL2/Hyper-V, where the CPUID instruction start()/write() use
// for serialization is trap-and-emulated by the hypervisor and costs tens of
// thousands of cycles of fixed overhead per call (empirically ~24k-44k
// cycles measured here). At a target of 1,000 cycles that overhead
// completely swamps the signal; at 500,000+ cycles it's a rounding error.
// Small/tight targets are intentionally NOT tested for that reason — this is
// a real characteristic of rdtsc-based timing under virtualization, not
// something a stricter bound would be correctly testing for.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Time/timer.hpp>

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

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

static void busyWaitCycles(uint64_t cycles) {
    uint64_t start = __rdtsc();
    uint64_t target = start + cycles;
    while (__rdtsc() < target) _mm_pause();
}

struct ParsedWrite { unsigned long long cycles; double ns; bool parsedOk; };

static ParsedWrite captureWrite(AstraLib::Time::Timer& timer, const std::string& label) {
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    timer.write(label);
    std::cout.rdbuf(old);

    std::string out = oss.str();
    ParsedWrite r{0, 0.0, false};
    size_t bracketEnd = out.find("] ");
    if (bracketEnd == std::string::npos) return r;
    std::string rest = out.substr(bracketEnd + 2);
    if (std::sscanf(rest.c_str(), "%llu cycles, ~%lf ns", &r.cycles, &r.ns) == 2) {
        r.parsedOk = true;
    }
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Output is parseable and the label round-trips into it verbatim.
// ─────────────────────────────────────────────────────────────────────────────
static void test_output_is_parseable_and_labeled(AstraLib::Time::Timer& timer) {
    timer.start();
    busyWaitCycles(1000000);
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    timer.write("MyCustomLabel");
    std::cout.rdbuf(old);
    std::string out = oss.str();

    CHECK_CTX(out.find("MyCustomLabel") != std::string::npos,
              "custom label missing from output: " << out);
    CHECK_CTX(out.find("cycles") != std::string::npos, "output missing 'cycles': " << out);
    CHECK_CTX(out.find("ns") != std::string::npos, "output missing 'ns': " << out);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Measured cycles must be at least the busy-wait target (write() runs
//    strictly after the loop exits, so it can only see equal-or-more).
// ─────────────────────────────────────────────────────────────────────────────
static void test_measured_at_least_target(AstraLib::Time::Timer& timer) {
    constexpr unsigned long long target = 2000000;
    timer.start();
    busyWaitCycles(target);
    ParsedWrite r = captureWrite(timer, "at_least_target");
    CHECK_CTX(r.parsedOk, "failed to parse Timer::write() output");
    CHECK_CTX(r.cycles >= target, "measured " << r.cycles << " cycles, expected >= " << target);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Monotonicity: a longer busy-wait must produce a larger measured value.
//    This is the real correctness signal here — it holds regardless of how
//    much fixed per-call overhead the virtualized CPUID adds, as long as
//    that overhead is roughly stable across nearby calls (which it is,
//    empirically: ~500k cycles apart is more than enough clearance).
// ─────────────────────────────────────────────────────────────────────────────
static void test_monotonic_with_wait_duration(AstraLib::Time::Timer& timer) {
    unsigned long long targets[] = {500000ULL, 5000000ULL, 50000000ULL};
    unsigned long long measured[3];
    for (int i = 0; i < 3; ++i) {
        timer.start();
        busyWaitCycles(targets[i]);
        ParsedWrite r = captureWrite(timer, "monotonic");
        CHECK_CTX(r.parsedOk, "failed to parse Timer::write() output at target " << targets[i]);
        measured[i] = r.cycles;
    }
    CHECK_CTX(measured[0] < measured[1],
              "target " << targets[0] << "->" << measured[0] << " not < target "
              << targets[1] << "->" << measured[1]);
    CHECK_CTX(measured[1] < measured[2],
              "target " << targets[1] << "->" << measured[1] << " not < target "
              << targets[2] << "->" << measured[2]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Self-consistency between the two numbers on one printed line: cycles/ns
//    implies a clock frequency, which must land in a sane range for any real
//    or virtualized modern CPU (loose bound: 0.1-10 GHz) — this indirectly
//    validates the ns computation without needing access to the private,
//    unexposed `ghz` field it depends on.
// ─────────────────────────────────────────────────────────────────────────────
static void test_ns_consistent_with_cycles(AstraLib::Time::Timer& timer) {
    timer.start();
    busyWaitCycles(10000000);
    ParsedWrite r = captureWrite(timer, "consistency");
    CHECK_CTX(r.parsedOk, "failed to parse Timer::write() output");
    CHECK_CTX(r.ns > 0.0, "non-positive ns: " << r.ns);
    double impliedGHz = double(r.cycles) / r.ns;
    CHECK_CTX(impliedGHz > 0.1 && impliedGHz < 10.0,
              "implied clock " << impliedGHz << " GHz from " << r.cycles
              << " cycles / " << r.ns << " ns is outside a sane range");
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. The same Timer instance (one calibration) must support many repeated
//    start()/write() cycles without drifting into nonsense.
// ─────────────────────────────────────────────────────────────────────────────
static void test_repeated_measurements_stay_sane(AstraLib::Time::Timer& timer) {
    constexpr unsigned long long target = 1000000;
    for (int i = 0; i < 20; ++i) {
        timer.start();
        busyWaitCycles(target);
        ParsedWrite r = captureWrite(timer, "repeat");
        CHECK_CTX(r.parsedOk, "iteration " << i << ": failed to parse output");
        CHECK_CTX(r.cycles >= target && r.cycles < target * 100,
                  "iteration " << i << ": measured " << r.cycles
                  << " cycles is out of sane range for target " << target);
    }
}

int main() {
    std::cout << "Timer test suite (calibrating GHz, ~1s)...\n" << std::endl;
    AstraLib::Time::Timer timer;   // one calibration, reused by every test below

    struct { const char* name; void (*fn)(AstraLib::Time::Timer&); } tests[] = {
        {"output_is_parseable_and_labeled", test_output_is_parseable_and_labeled},
        {"measured_at_least_target",        test_measured_at_least_target},
        {"monotonic_with_wait_duration",    test_monotonic_with_wait_duration},
        {"ns_consistent_with_cycles",       test_ns_consistent_with_cycles},
        {"repeated_measurements_stay_sane", test_repeated_measurements_stay_sane},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        long before = g_failures;
        t.fn(timer);
        std::cout << (g_failures == before ? "[  OK  ] " : "[ BAD  ] ") << t.name << std::endl;
    }

    if (g_failures == 0) { std::cout << "\nALL TIMER TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
