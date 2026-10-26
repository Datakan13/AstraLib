// ─────────────────────────────────────────────────────────────────────────────
// Shared harness for the comparison benchmarks in bench/.
//
// These binaries put AstraLib's primitives head to head with established
// open-source equivalents under identical conditions: same thread placement,
// same item counts, same capacity where the competitor has one, same
// accounting. They are benchmarks, not tests — the only thing they ASSERT is
// that the work actually happened (exact item/acquisition accounting), so a
// broken competitor adapter can't post a flattering number by doing nothing.
//
// Methodology carried over from tests/AtomicRingBufferPerformanceTest.cpp
// (see its header for the evidence behind each point):
//
//   * Threads are pinned ONE PER PHYSICAL CORE. Never SMT siblings: two
//     spinning threads on one core's hyperthreads measure the core, not the
//     code. Configurations that need more cores than exist are SKIPPED, not
//     oversubscribed — spinning code oversubscribed doesn't run slowly, it
//     falls off a cliff.
//   * One discarded warmup, then REPS timed runs; report MEDIAN and RANGE.
//     Rows whose max/min exceeds 1.5x are flagged UNSTABLE and must be read
//     as a range.
//   * Within one run, competitors are interleaved per rep (A B C A B C ...)
//     rather than run back to back (A A A B B B ...). This dev box loses
//     roughly 2x frequency under sustained all-core load; back-to-back
//     ordering would hand whoever runs first a systematic advantage.
//   * Cross-library comparisons WITHIN one run are sound. Absolute numbers
//     ACROSS runs are soft on this machine (hybrid P/E cores behind WSL2's
//     synthetic topology). Compare the "vs Astra" column, not raw M ops/s
//     between two invocations.
//
// Knobs:
//   ASTRA_BENCH_SCALE=N   divide work by N (quick smoke run)
//   ASTRA_BENCH_MULT=N    multiply work by N (longer, steadier runs)
//   ASTRA_BENCH_REPS=N    timed reps per configuration (default 7)
//   ASTRA_BENCH_VERBOSE=1 print every rep, not just the summary
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>
#include <x86intrin.h>

namespace Bench {

// ── CHECK machinery ─────────────────────────────────────────────────────────
inline std::atomic<long> g_failures{0};

#define BENCH_CHECK(cond, ...)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            Bench::g_failures.fetch_add(1, std::memory_order_relaxed);         \
            std::fprintf(stderr, "[FAIL] %s:%d  CHECK(" #cond ")  ",           \
                         __FILE__, __LINE__);                                  \
            std::fprintf(stderr, __VA_ARGS__);                                 \
            std::fprintf(stderr, "\n");                                        \
        }                                                                      \
    } while (0)

template <typename T>
inline void doNotOptimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── watchdog: several of these primitives block by spinning, so a bug ───────
// (in AstraLib or in an adapter) presents as a hang, not an error.
inline std::atomic<const char*> g_benchName{"<none>"};
inline std::atomic<int64_t>     g_deadlineNs{INT64_MAX};

inline void watchdogLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int64_t dl = g_deadlineNs.load(std::memory_order_acquire);
        if (dl != INT64_MAX && nowNs() > dl) {
            std::fprintf(stderr,
                "\n[WATCHDOG] '%s' exceeded its deadline — deadlocked or "
                "livelocked.\n", g_benchName.load());
            std::fflush(nullptr);
            std::_Exit(3);
        }
    }
}

inline void startWatchdog() { std::thread(watchdogLoop).detach(); }

struct Deadline {
    Deadline(const char* name, int seconds) {
        g_benchName.store(name);
        g_deadlineNs.store(nowNs() + int64_t(seconds) * 1'000'000'000);
    }
    ~Deadline() { g_deadlineNs.store(INT64_MAX); }
};

// ── knobs ───────────────────────────────────────────────────────────────────
inline uint64_t g_scale = 1, g_mult = 1;
inline int g_reps = 7;
inline bool g_verbose = false;

inline uint64_t scaled(uint64_t base, uint64_t minimum = 1000) {
    uint64_t v = base * g_mult / g_scale;
    return v < minimum ? minimum : v;
}

inline void readKnobs() {
    auto num = [](const char* k, uint64_t def) {
        const char* s = std::getenv(k);
        if (!s) return def;
        uint64_t v = std::strtoull(s, nullptr, 10);
        return v ? v : def;
    };
    g_scale   = num("ASTRA_BENCH_SCALE", 1);
    g_mult    = num("ASTRA_BENCH_MULT", 1);
    g_reps    = int(num("ASTRA_BENCH_REPS", 7));
    g_verbose = std::getenv("ASTRA_BENCH_VERBOSE") != nullptr;
}

// ── topology: one logical CPU per PHYSICAL core ─────────────────────────────
inline std::vector<unsigned> g_physCpus;

inline void detectTopology() {
    const unsigned hw = std::thread::hardware_concurrency();
    std::vector<unsigned> leaders;
    for (unsigned c = 0; c < hw; ++c) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", c);
        std::FILE* f = std::fopen(path, "r");
        if (!f) { leaders.clear(); break; }
        unsigned leader = c;
        if (std::fscanf(f, "%u", &leader) != 1) leader = c;
        std::fclose(f);
        if (std::find(leaders.begin(), leaders.end(), leader) == leaders.end())
            leaders.push_back(leader);
    }
    if (leaders.empty())
        for (unsigned c = 0; c < (hw ? hw : 1u); ++c) leaders.push_back(c);
    g_physCpus = leaders;
}

inline unsigned coreCount() {
    return g_physCpus.empty() ? 1u : unsigned(g_physCpus.size());
}

inline void pinToCore(unsigned slot) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_physCpus[slot % g_physCpus.size()], &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);   // best effort
}

// Keeps the coordinating thread off every core a worker is using.
inline void pinCoordinatorAway(unsigned workersInUse) {
    if (coreCount() > workersInUse) pinToCore(coreCount() - 1);
}

inline void unpin() {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned c = 0; c < std::thread::hardware_concurrency(); ++c) CPU_SET(c, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// ── TSC → ns, for per-sample latency where steady_clock is too coarse ───────
inline double g_tscPerNs = 1.0;

inline void calibrateTsc() {
    const int64_t n0 = nowNs();
    const uint64_t t0 = __rdtsc();
    while (nowNs() - n0 < 200'000'000) _mm_pause();
    const uint64_t t1 = __rdtsc();
    g_tscPerNs = double(t1 - t0) / double(nowNs() - n0);
}

inline double cyclesToNs(uint64_t c) { return double(c) / g_tscPerNs; }

// ── pinned parallel run ─────────────────────────────────────────────────────
// Spawns body(0..n-1), each pinned to its own physical core, releases them
// together, and returns the wall time in ns from release to last finisher.
inline int64_t runPinned(unsigned n, const std::function<void(unsigned)>& body) {
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        ts.emplace_back([&, i] {
            pinToCore(i);
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) _mm_pause();
            body(i);
        });
    pinCoordinatorAway(n);
    while (ready.load(std::memory_order_acquire) < n) _mm_pause();
    const int64_t t0 = nowNs();
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    const int64_t t1 = nowNs();
    unpin();
    return t1 - t0;
}

// ── stats ───────────────────────────────────────────────────────────────────
struct Stat { double median, lo, hi; };

inline Stat summarize(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    return {med, v.front(), v.back()};
}

inline double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = size_t(p * double(sorted.size() - 1) + 0.5);
    return sorted[std::min(idx, sorted.size() - 1)];
}

// ── table output ────────────────────────────────────────────────────────────
// Every comparison table is: one row per competitor, throughput median with
// range, and a ratio against the AstraLib row of the same table (>1.00x means
// the competitor is FASTER than AstraLib on that row).
struct Row {
    std::string name;
    std::string note;
    Stat stat;           // in the table's unit
    bool skipped = false;
};

inline void printTable(const char* title, const char* unit,
                       const std::vector<Row>& rows, bool higherIsBetter = true) {
    std::printf("\n  %s\n", title);
    double astra = 0;
    for (const Row& r : rows)
        if (!r.skipped && r.name.rfind("AstraLib", 0) == 0) { astra = r.stat.median; break; }
    for (const Row& r : rows) {
        if (r.skipped) {
            std::printf("    %-30s %s\n", r.name.c_str(), r.note.c_str());
            continue;
        }
        const double ratio = astra > 0
            ? (higherIsBetter ? r.stat.median / astra : astra / r.stat.median) : 0;
        const bool unstable = r.stat.lo > 0 && r.stat.hi / r.stat.lo > 1.5;
        std::printf("    %-30s %9.2f %-8s range %8.2f-%-8.2f  %5.2fx vs Astra%s%s%s\n",
                    r.name.c_str(), r.stat.median, unit, r.stat.lo, r.stat.hi, ratio,
                    unstable ? "  << UNSTABLE" : "",
                    r.note.empty() ? "" : "  ", r.note.c_str());
    }
}

// Runs every competitor once per rep, interleaved, after one discarded warmup
// pass of each. `fns[i]()` returns one sample in the table's unit.
inline std::vector<Stat> interleaved(const std::vector<std::function<double()>>& fns,
                                     const std::vector<std::string>& names) {
    for (auto& f : fns) f();   // warmup: first-touch, frequency ramp
    std::vector<std::vector<double>> samples(fns.size());
    for (int r = 0; r < g_reps; ++r)
        for (size_t i = 0; i < fns.size(); ++i) {
            const double s = fns[i]();
            samples[i].push_back(s);
            if (g_verbose)
                std::printf("      rep %d  %-28s %.3f\n", r, names[i].c_str(), s);
        }
    std::vector<Stat> out;
    for (auto& s : samples) out.push_back(summarize(s));
    return out;
}

inline void printPreamble(const char* what) {
    std::printf("%s\n", what);
    std::printf("  physical cores: %u   pinned cpus:", coreCount());
    for (unsigned c : g_physCpus) std::printf(" %u", c);
    std::printf("\n  TSC %.3f GHz   reps %d (median, min-max)   scale x%llu/%llu\n",
                g_tscPerNs, g_reps,
                (unsigned long long)g_mult, (unsigned long long)g_scale);
}

inline int finish() {
    if (g_failures.load()) {
        std::printf("\n%ld ACCOUNTING CHECK(S) FAILED — numbers above are not valid.\n",
                    g_failures.load());
        return 1;
    }
    std::printf("\nAll accounting checks passed. Numbers are reported, not gated.\n");
    return 0;
}

inline void init() {
    readKnobs();
    detectTopology();
    calibrateTsc();
    startWatchdog();
}

}  // namespace Bench
