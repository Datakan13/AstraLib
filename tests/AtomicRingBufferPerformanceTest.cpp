// ─────────────────────────────────────────────────────────────────────────────
// Performance characterization for AstraLib::Buffers::AtomicRingBuffer.
//
// This is a BENCHMARK, not a pass/fail test. It reports numbers; it asserts
// only that the work actually happened (exact item accounting) and that
// throughput cleared an absurdly low floor. Absolute throughput is NOT
// gated: this suite's CI runner is a shared 2-vCPU GitHub host whose
// run-to-run variance is several hundred percent, so a real threshold would
// produce far more false failures than regressions caught. It is registered
// under the ctest label `perf` and excluded from the default run — use
// `ctest -L perf` to run it deliberately.
//
// What it measures:
//
//   1. spsc_throughput    — 1P/1C steady state. The ceiling: no ticket
//                           contention, so this is the cost of the protocol
//                           itself (2 fetch_adds, 2 seq loads, 2 seq stores).
//   2. mpmc_scaling       — 1P/1C, 2P/2C, 4P/4C at a fixed total item count.
//                           writeTicket and readTicket are each a single
//                           shared cache line hit with fetch_add(acq_rel) by
//                           every thread on that side, so the question this
//                           answers is whether aggregate throughput goes
//                           DOWN as threads are added.
//   3. handoff_latency    — enqueue→dequeue latency distribution
//                           (p50/p99/p99.9/max), not just a mean. A spin
//                           queue's tail is the number that matters, and a
//                           mean hides it completely. Measured as a ping-pong
//                           round trip so both timestamps are taken by the
//                           same thread on the same core — see the section
//                           comment for why the obvious one-way version is
//                           not sound.
//   4. batch_vs_single    — batchDequeue(N) against N x dequeue(). That
//                           method exists purely to amortize the per-item
//                           ticket round trip, and it costs an MPSC-only
//                           usage restriction, so the payoff is worth
//                           quantifying.
//   5. payload_size       — 4-byte, 56-byte and 120-byte payloads. The
//                           copy/move happens while the slot is claimed but
//                           not yet published, so payload size directly
//                           widens that window. 120 bytes is what Slot's own
//                           static_assert permits (128 - sizeof(seq)); note
//                           that pushes sizeof(Slot) to 128, i.e. two cache
//                           lines per slot.
//
// ── Timing methodology ──────────────────────────────────────────────────────
//
// Two clocks, deliberately:
//
//   * AstraLib::Time::Timer for the aggregate throughput runs. Timer::start()
//     and getTimeCycles() each issue __cpuid to serialize, which under this
//     repo's WSL2/Hyper-V target is trap-and-emulated at roughly 24k-44k
//     cycles per call (measured and documented in TimerTest.cpp's header).
//     Across a multi-million-item run that overhead is a rounding error, so
//     this is exactly the regime Timer is accurate in.
//
//   * Raw __rdtsc() for the per-item latency samples. An enqueue is on the
//     order of tens of nanoseconds; timing it with Timer would measure the
//     ~10us instrument, not the queue. Cycle deltas are converted to ns with
//     a TSC frequency calibrated here against steady_clock (200ms, no
//     sleep(1)); constant_tsc/nonstop_tsc are checked at startup because
//     without them that conversion drifts with core frequency.
//
// Every measurement: one discarded warmup run (cold caches, first-touch page
// faults on the slot array, CPU frequency ramp), then REPS timed runs
// reporting the MEDIAN and the full min-max RANGE. A single run is noise, a
// mean alone is misleading under contention, and a median alone hides the
// finding below.
//
// ── Read the range, not just the median ─────────────────────────────────────
//
// Rows flagged UNSTABLE (best/worst > 1.5x) must be read as a range, not a
// number. Nine reps of identical 1P/1C work produced:
//
//     26.8  39.3  135.2  98.2  113.8  119.4  132.2  47.7  58.1  M ops/s
//
// THE CAUSE IS NOT KNOWN. Two explanations were written here as established
// fact and both were wrong (ring occupancy; heterogeneous P/E cores behind
// WSL2's synthetic topology — the latter died when a reverse-order pair
// sweep showed the apparent per-core ranking does not reproduce). See
// docs/tests.md finding 9 for the full retraction and evidence. Do not add a
// third explanation here without an experiment that isolates it.
//
// What the two probes below positively establish on this dev box:
// single-thread speed varies by only 1.09x across cores, and pairwise
// handoff cost by only 1.46x. Neither can produce a 5x swing, and ordering
// cores by either metric did not reduce the bimodality — which is why the
// probe only reorders when the spread exceeds 1.5x.
//
// Separately verified and real: sustained all-core load costs roughly 2x in
// frequency and keeps climbing, so later reps run slower than earlier ones.
// The end-of-run drift readout reports it rather than correcting for it;
// there is no honest way to rescale after the fact.
//
// Practical: trust cross-configuration comparisons WITHIN one run; treat
// absolute numbers ACROSS runs as soft. The tight rows (4P/4C, 120-byte
// payloads) are the trustworthy ones.
//
// Dequeued values are fed through an optimizer barrier — otherwise the
// result of dequeue() is dead and the compiler is free to delete parts of
// the loop being measured.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Worker threads are pinned one per PHYSICAL core, and configurations that
// would need more cores than exist are skipped rather than oversubscribed.
// All three parts of that matter:
//
//   * Oversubscribing spinning code is catastrophic, not proportional — this
//     suite already lost three tests to it on the 2-vCPU CI runner (see
//     docs/tests.md, "Automatic scaling on small machines"). A benchmark
//     that oversubscribes doesn't report a slow number, it hangs.
//   * Unpinned threads aren't comparable run to run. Whether two threads
//     land on SMT siblings, the same L3, or different sockets moves MPMC
//     numbers by several x.
//   * Physical, not logical. Two threads spinning on _mm_pause on SMT
//     siblings share one core's execution resources. See detectTopology()
//     for what that did to these numbers before it was fixed.
//
// Knobs (all default to the values used for the numbers quoted above):
//   ASTRA_PERF_SCALE=N   divide item counts by N (use under sanitizers)
//   ASTRA_PERF_MULT=N    multiply item counts by N (longer runs)
//   ASTRA_PERF_REPS=N    number of timed reps per configuration
//   ASTRA_PERF_VERBOSE=1 print every rep's throughput, not just the summary
//   ASTRA_PERF_NOPROBE=1 skip the core-speed probe and use sysfs enumeration
//                        order (reproducibility/debugging escape hatch)
// ─────────────────────────────────────────────────────────────────────────────

// ── TEMPORARY: batchDequeue benchmark disabled ──────────────────────────────
// batchDequeue() is mid-rework (claiming the whole batch with one
// readTicket.fetch_add instead of one per item). Section [4] is excluded
// until that lands. Flip to 0 to bring it back.
#define ASTRA_SKIP_BATCH_DEQUEUE_TESTS 1

#include <AstraLib/Buffers/atomicRingBuffer.hpp>
#include <AstraLib/Time/timer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace RB = AstraLib::Buffers;

// ── layout expectations these numbers are interpreted against ───────────────
struct Payload56  { char b[56];  };
struct Payload120 { char b[120]; };
static_assert(sizeof(RB::Slot<int>)        == 64,  "Slot<int> should be one cache line");
static_assert(sizeof(RB::Slot<Payload56>)  == 64,  "56B payload should still fit one line");
static_assert(sizeof(RB::Slot<Payload120>) == 128, "120B payload spans two cache lines");

// ── CHECK machinery (same shape as the rest of the suite) ───────────────────
static std::atomic<long> g_failures{0};

#define CHECK_CTX(cond, ctx)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures.fetch_add(1, std::memory_order_relaxed);                \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__              \
                      << "  CHECK(" #cond ")  " << ctx << "\n";                \
        }                                                                      \
    } while (0)
#define CHECK(cond) CHECK_CTX(cond, "")

// A floor so low it can only mean something is structurally broken (a
// pathological spin, a lost wakeup, accidental serialization) rather than a
// slow machine. The real regression signal is the printed table.
static const double ABSURD_FLOOR_OPS = 100000.0;

// ── watchdog: this queue blocks, so a bug presents as a hang ────────────────
static std::atomic<const char*> g_benchName{"<none>"};
static std::atomic<int64_t>     g_deadlineNs{INT64_MAX};
static std::atomic<uint64_t>    g_produced{0};
static std::atomic<uint64_t>    g_consumed{0};

static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void watchdogLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int64_t dl = g_deadlineNs.load(std::memory_order_acquire);
        if (dl != INT64_MAX && nowNs() > dl) {
            std::fprintf(stderr,
                "\n[WATCHDOG] benchmark '%s' exceeded its deadline — the queue "
                "is deadlocked or livelocked.\n"
                "[WATCHDOG] progress: produced=%llu consumed=%llu\n",
                g_benchName.load(),
                (unsigned long long)g_produced.load(),
                (unsigned long long)g_consumed.load());
            std::fflush(nullptr);
            std::_Exit(3);
        }
    }
}

struct Deadline {
    explicit Deadline(const char* name, int seconds) {
        g_benchName.store(name);
        g_produced.store(0);
        g_consumed.store(0);
        g_deadlineNs.store(nowNs() + int64_t(seconds) * 1'000'000'000);
    }
    ~Deadline() { g_deadlineNs.store(INT64_MAX); }
};

// ── helpers ─────────────────────────────────────────────────────────────────
template <typename T>
static inline void doNotOptimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

static uint64_t g_scale = 1;   // ASTRA_PERF_SCALE: divide item counts
static uint64_t g_mult  = 1;   // ASTRA_PERF_MULT:  multiply item counts
static uint64_t scaled(uint64_t base, uint64_t minimum = 10000) {
    uint64_t v = (base * g_mult) / g_scale;
    return v < minimum ? minimum : v;
}

// One logical CPU per PHYSICAL core, not per hardware thread.
//
// This distinction is not a refinement, it was the single largest source of
// error in this file. Pinning a producer to cpu0 and a consumer to cpu1 put
// them on SMT siblings of one physical core on this machine (verified via
// /sys/.../topology/thread_siblings_list, which reports "0-1" for both), so
// two threads spinning on _mm_pause were fighting over one core's execution
// resources. That made the SPSC figure both slow and unstable: 35 M ops/s
// median against 130 M best across five runs of identical work, while the
// byte-for-byte identical 1P/1C configuration elsewhere in the same run
// reported 116 M. Sibling placement, not the queue, was being measured.
static std::vector<unsigned> g_physCpus;

static void detectTopology() {
    const unsigned hw = std::thread::hardware_concurrency();
    std::vector<unsigned> seenLeaders;
    for (unsigned c = 0; c < hw; ++c) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", c);
        std::FILE* f = std::fopen(path, "r");
        if (!f) { seenLeaders.clear(); break; }
        unsigned leader = c;
        if (std::fscanf(f, "%u", &leader) != 1) leader = c;
        std::fclose(f);
        // The lowest-numbered sibling stands for its physical core; take each
        // such leader once and every pinned thread lands on its own core.
        if (std::find(seenLeaders.begin(), seenLeaders.end(), leader) == seenLeaders.end())
            seenLeaders.push_back(leader);
    }
    if (seenLeaders.empty())
        for (unsigned c = 0; c < (hw ? hw : 1u); ++c) seenLeaders.push_back(c);
    g_physCpus = seenLeaders;
}

// Cap so producers + consumers together never exceed the physical core count.
// A configuration needing more is skipped rather than run oversubscribed — a
// number produced by 8 spinning threads on 2 cores is not a slow measurement,
// it is a meaningless one.
static unsigned coreCount() {
    return g_physCpus.empty() ? 1u : unsigned(g_physCpus.size());
}

static void pinRawCpu(unsigned cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void pinToCore(unsigned slot) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_physCpus[slot % g_physCpus.size()], &set);
    // Best effort: a restricted cpuset or container just means the numbers
    // are noisier, not wrong, so a failure here is ignored.
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Parks the coordinating thread away from every worker slot in use, so its
// own spin-then-join never shares a core with the code being measured.
static void pinCoordinatorAway(unsigned workersInUse) {
    if (coreCount() > workersInUse) pinToCore(coreCount() - 1);
}

// ── pairwise handoff probe: find cores that TALK cheaply ────────────────────
//
// First attempt here measured single-thread speed per core and used the
// fastest first. That was the wrong axis and the probe said so: it reported
// a 1.09x spread across all ten cores (near-identical), while a pinned SPSC
// pair sweep had shown a stable 6-8x spread BY PAIR (cpu0+cpu2 ~15-28
// M ops/s, cpu16+cpu18 ~118-127). Both results are correct and together they
// rule out core speed as the cause.
//
// What a ring-buffer handoff actually costs is a cache line moving between
// two specific cores, and that is a property of the PAIR, not of either core
// alone — cache topology (on a hybrid part, E-cores share an L2 per cluster;
// cross-cluster and cross-type transfers are far more expensive) decides it.
// A single-threaded loop cannot observe that no matter how carefully it is
// measured.
//
// So probe pairs. For every candidate pair, run a short ping-pong through two
// SIZE=2 buffers and take the cheapest round trip observed. Then order cores
// greedily: start from the cheapest-talking pair, and repeatedly append the
// core with the lowest worst-case cost to everything already chosen. The
// two-thread configurations then get two cores that actually talk cheaply —
// which is the whole point — and the wider ones degrade gracefully.
//
// Minimum rather than median: this measures a floor, and interference can
// only ever make a pair look worse than it is.
struct CoreProbe { unsigned cpu; uint64_t cycles; };
static std::vector<CoreProbe> g_probe;          // retained: per-core drift baseline
static uint64_t g_probeBaselineCycles = 0;
static uint64_t g_bestPairCycles = 0, g_worstPairCycles = 0;
static bool g_probeReordered = false;

static uint64_t probeWorkload() {
    const uint64_t t0 = __rdtsc();
    uint64_t x = 0;
    for (uint64_t i = 0; i < 8'000'000; ++i) { x += i; doNotOptimize(x); }
    doNotOptimize(x);
    return __rdtsc() - t0;
}

static uint64_t probeCpu(unsigned cpu, int passes) {
    uint64_t best = UINT64_MAX;
    std::thread t([&] {
        pinRawCpu(cpu);
        for (int i = 0; i < passes; ++i) best = std::min(best, probeWorkload());
    });
    t.join();
    return best;
}

// Cheapest observed ping-pong round trip between two cpus, in TSC cycles.
static uint64_t pairLatency(unsigned cpuA, unsigned cpuB, uint64_t iters) {
    RB::AtomicRingBuffer<uint64_t, 2> req, rep;
    std::atomic<bool> go{false};
    std::atomic<int> ready{0};
    std::thread echo([&] {
        pinRawCpu(cpuB);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) _mm_pause();
        for (uint64_t i = 0; i < iters; ++i) rep.enqueue(req.dequeue());
    });
    pinRawCpu(cpuA);
    while (ready.load(std::memory_order_acquire) < 1) _mm_pause();
    go.store(true, std::memory_order_release);
    uint64_t best = UINT64_MAX;
    for (uint64_t i = 0; i < iters; ++i) {
        const uint64_t t0 = __rdtsc();
        req.enqueue(i);
        const uint64_t v = rep.dequeue();
        const uint64_t d = __rdtsc() - t0;
        doNotOptimize(v);
        if (i > iters / 10 && d < best) best = d;   // skip the cold prefix
    }
    echo.join();
    return best;
}

static void probeAndOrderCores() {
    const size_t n = g_physCpus.size();
    if (n < 2) return;
    const std::vector<unsigned> cpus = g_physCpus;

    std::vector<std::vector<uint64_t>> lat(n, std::vector<uint64_t>(n, 0));
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            lat[a][b] = lat[b][a] = pairLatency(cpus[a], cpus[b], 4000);

    // Seed with the cheapest-talking pair.
    size_t bestA = 0, bestB = 1;
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            if (lat[a][b] < lat[bestA][bestB]) { bestA = a; bestB = b; }
    g_bestPairCycles = lat[bestA][bestB];
    g_worstPairCycles = 0;
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            g_worstPairCycles = std::max(g_worstPairCycles, lat[a][b]);

    std::vector<size_t> chosen{bestA, bestB};
    std::vector<bool> used(n, false);
    used[bestA] = used[bestB] = true;
    while (chosen.size() < n) {
        size_t bestIdx = n;
        uint64_t bestCost = UINT64_MAX;
        for (size_t c = 0; c < n; ++c) {
            if (used[c]) continue;
            uint64_t worst = 0;
            for (size_t k : chosen) worst = std::max(worst, lat[c][k]);
            if (worst < bestCost) { bestCost = worst; bestIdx = c; }
        }
        used[bestIdx] = true;
        chosen.push_back(bestIdx);
    }

    // Only ACT on the probe when it found a real difference. Measured spread
    // on this dev box is ~1.46x, and a reverse-order sweep showed the
    // apparent per-pair ranking does not reproduce — reordering on that would
    // be fitting noise. On a machine with genuinely non-uniform cache
    // topology the spread is large and the ordering is worth taking.
    if (double(g_worstPairCycles) / double(g_bestPairCycles) > 1.5) {
        g_physCpus.clear();
        for (size_t idx : chosen) g_physCpus.push_back(cpus[idx]);
        g_probeReordered = true;
    }

    // Drift baseline is taken on the core the benchmarks will use first.
    g_probe.clear();
    g_probe.push_back({g_physCpus[0], probeCpu(g_physCpus[0], 3)});
    g_probeBaselineCycles = g_probe.front().cycles;
}

static bool g_invariantTsc = false;
static double g_tscGhz = 0.0;

static void detectInvariantTsc() {
    std::FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[4096];
    bool constant = false, nonstop = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "flags", 5) != 0) continue;
        constant = std::strstr(line, "constant_tsc") != nullptr;
        nonstop  = std::strstr(line, "nonstop_tsc")  != nullptr;
        break;
    }
    std::fclose(f);
    g_invariantTsc = constant && nonstop;
}

// Calibrate TSC ticks per nanosecond against steady_clock. Deliberately not
// reusing Timer's calibration: Timer keeps its GHz private and pays a
// blocking sleep(1) for it, and this only needs 200ms to be accurate enough
// for percentile reporting.
static void calibrateTsc() {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t c1 = __rdtsc();
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_tscGhz = double(c1 - c0) / ns;
}

static double cyclesToNs(uint64_t cycles) { return double(cycles) / g_tscGhz; }

// Median of the timed reps, not the mean: under contention a single
// descheduled run skews a mean badly, and the min alone is a best-case
// fiction.
static double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static double percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = size_t(p * double(sorted.size() - 1));
    return cyclesToNs(sorted[idx]);
}

static const int WARMUP = 1;
static int g_reps = 15;   // ASTRA_PERF_REPS

static bool g_verbose = false;

static void reportThroughput(const char* name, const char* config,
                             uint64_t items, const std::vector<double>& nsRuns) {
    if (g_verbose) {
        std::printf("      [reps] %s %s:", name, config);
        for (double ns : nsRuns) std::printf(" %.1f", double(items) / (ns / 1e9) / 1e6);
        std::printf("  M ops/s\n");
    }
    const double medNs = median(nsRuns);
    const double minNs = *std::min_element(nsRuns.begin(), nsRuns.end());
    const double maxNs = *std::max_element(nsRuns.begin(), nsRuns.end());
    const double medOps  = double(items) / (medNs / 1e9);
    const double bestOps = double(items) / (minNs / 1e9);
    const double worstOps = double(items) / (maxNs / 1e9);

    // Report the SPREAD, never the median alone. Some configurations here are
    // genuinely bimodal (see the file header), and a lone median would
    // present a ~5x-unstable measurement with exactly the same authority as
    // a ~1.05x-stable one.
    const double spread = (worstOps > 0.0) ? bestOps / worstOps : 0.0;
    std::printf("  %-22s %-14s %8.1f M ops/s  %7.2f ns/op   range %6.1f-%6.1f  %s\n",
                name, config, medOps / 1e6, medNs / double(items),
                worstOps / 1e6, bestOps / 1e6,
                spread > 1.5 ? "<< UNSTABLE" : "");
    CHECK_CTX(medOps > ABSURD_FLOOR_OPS,
              name << " " << config << ": " << medOps << " ops/s is below the "
              "structural-failure floor of " << ABSURD_FLOOR_OPS << " ops/s — "
              "this is not a slow machine, something is spinning pathologically");
}

// ─────────────────────────────────────────────────────────────────────────────
// 1 & 2. Throughput: SPSC ceiling, then MPMC scaling at fixed total work.
//
// Termination protocol: each producer emits exactly TOTAL/P items and each
// consumer removes exactly TOTAL/C. With a blocking queue there is no other
// safe way to stop — a consumer that takes one ticket too many spins forever
// on a slot that will never be published.
//
// Accounting: producer p emits values p*perProducer .. (p+1)*perProducer-1,
// so the sum over every dequeued value must equal the sum 0..TOTAL-1. That
// catches loss and duplication, which is the failure mode that would
// otherwise make a benchmark report a wonderful number for doing less work.
// ─────────────────────────────────────────────────────────────────────────────
template <std::size_t SIZE>
static double runThroughputOnce(int producers, int consumers, uint64_t total) {
    RB::AtomicRingBuffer<uint64_t, SIZE> q;
    const uint64_t perProducer = total / uint64_t(producers);
    const uint64_t perConsumer = total / uint64_t(consumers);

    std::vector<std::thread> threads;
    std::vector<uint64_t> sums(size_t(consumers), 0);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            pinToCore(unsigned(producers + c));
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) _mm_pause();
            uint64_t local = 0;
            for (uint64_t i = 0; i < perConsumer; ++i) {
                uint64_t v = q.dequeue();
                doNotOptimize(v);
                local += v;
            }
            sums[size_t(c)] = local;
            g_consumed.fetch_add(perConsumer, std::memory_order_relaxed);
        });
    }
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            pinToCore(unsigned(p));
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) _mm_pause();
            const uint64_t base = uint64_t(p) * perProducer;
            for (uint64_t i = 0; i < perProducer; ++i) q.enqueue(base + i);
            g_produced.fetch_add(perProducer, std::memory_order_relaxed);
        });
    }

    pinCoordinatorAway(unsigned(producers + consumers));
    while (ready.load(std::memory_order_acquire) < producers + consumers) _mm_pause();
    const int64_t t0 = nowNs();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    const double elapsedNs = double(nowNs() - t0);

    uint64_t got = 0;
    for (uint64_t s : sums) got += s;
    const uint64_t expected = total * (total - 1) / 2;
    CHECK_CTX(got == expected,
              "item accounting: dequeued value sum " << got << " != expected "
              << expected << " for " << producers << "P/" << consumers << "C — "
              "items were lost or delivered twice, so the timing above is "
              "measuring less work than it claims");
    return elapsedNs;
}

template <std::size_t SIZE>
static void benchThroughput(const char* name, int producers, int consumers,
                            uint64_t total) {
    char config[32];
    std::snprintf(config, sizeof(config), "%dP/%dC", producers, consumers);

    if (unsigned(producers + consumers) > coreCount()) {
        std::printf("  %-22s %-14s SKIPPED (needs %d cores, have %u)\n",
                    name, config, producers + consumers, coreCount());
        return;
    }
    // Round total down to a common multiple so both sides divide evenly —
    // an uneven split would leave a consumer waiting on an item nobody sends.
    const uint64_t lcm = uint64_t(producers) * uint64_t(consumers);
    total = (total / lcm) * lcm;

    Deadline dl(name, 120);
    for (int i = 0; i < WARMUP; ++i) runThroughputOnce<SIZE>(producers, consumers, total);
    std::vector<double> runs;
    for (int i = 0; i < g_reps; ++i)
        runs.push_back(runThroughputOnce<SIZE>(producers, consumers, total));
    reportThroughput(name, config, total, runs);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Handoff latency distribution, measured as a ping-pong round trip.
//
// The obvious design — producer stamps __rdtsc() before enqueue, consumer
// stamps after dequeue, subtract — is NOT sound, and was tried here first:
// it reported a max of 6.8e18 ns, i.e. an unsigned wrap of a NEGATIVE delta,
// meaning the consumer's timestamp came out earlier than the producer's.
// Even with constant_tsc/nonstop_tsc, per-core TSCs are only synchronized to
// within a small offset, and rdtsc is not a serializing instruction, so a
// sub-microsecond one-way interval measured across two cores can legitimately
// come back negative. Clamping or discarding those samples would be worse
// than useless: the skew biases every sample, not only the ones that cross
// zero, so the surviving distribution would be quietly wrong instead of
// visibly broken.
//
// Instead: this thread stamps, sends through `req`, an echo thread bounces it
// straight back through `rep`, and this thread stamps again. Both timestamps
// are taken by one thread pinned to one core, so there is no cross-core
// comparison anywhere and the measurement needs no correction.
//
// That gives a ROUND TRIP: two full enqueue→dequeue handoffs. Half of it is
// reported as the one-way estimate, which assumes the two directions are
// symmetric — they are, here, since both queues are the same type and both
// threads are pinned to dedicated cores.
//
// Both queues are SIZE=2, the minimum the buffer allows. Depth can never
// exceed one item, so no sample can include time spent waiting behind a
// backlog — that would be queueing delay, a different quantity from the
// handoff cost this is after.
// ─────────────────────────────────────────────────────────────────────────────
struct Stamp { uint64_t tsc; };

static void benchLatency(uint64_t samples) {
    const char* name = "handoff_latency";
    Deadline dl(name, 120);
    RB::AtomicRingBuffer<Stamp, 2> req;
    RB::AtomicRingBuffer<Stamp, 2> rep;
    std::vector<uint64_t> rtt;
    rtt.reserve(samples);

    std::atomic<bool> go{false};
    std::thread echo([&] {
        pinToCore(1);
        while (!go.load(std::memory_order_acquire)) _mm_pause();
        for (uint64_t i = 0; i < samples; ++i) {
            rep.enqueue(req.dequeue());
            g_consumed.store(i + 1, std::memory_order_relaxed);
        }
    });

    pinToCore(0);
    go.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < samples; ++i) {
        const uint64_t t0 = __rdtsc();
        req.enqueue(Stamp{t0});
        Stamp s = rep.dequeue();
        const uint64_t t1 = __rdtsc();
        doNotOptimize(s);
        rtt.push_back(t1 - t0);
        g_produced.store(i + 1, std::memory_order_relaxed);
    }
    echo.join();

    CHECK_CTX(rtt.size() == samples,
              "latency sampling: collected " << rtt.size() << " of "
              << samples << " samples");

    // The first samples are cold-cache/page-fault outliers on every run.
    const size_t warm = rtt.size() / 10;
    std::vector<uint64_t> s(rtt.begin() + long(warm), rtt.end());
    std::sort(s.begin(), s.end());

    std::printf("  %-22s %-14s p50 %7.0f ns  p99 %8.0f ns  p99.9 %9.0f ns  max %10.0f ns%s\n",
                name, "round trip",
                percentile(s, 0.50), percentile(s, 0.99),
                percentile(s, 0.999), cyclesToNs(s.back()),
                g_invariantTsc ? "" : "  [TSC not invariant: ns scale approximate]");
    std::printf("  %-22s %-14s p50 %7.0f ns  p99 %8.0f ns  p99.9 %9.0f ns\n",
                "", "one-way (rtt/2)",
                percentile(s, 0.50) / 2.0, percentile(s, 0.99) / 2.0,
                percentile(s, 0.999) / 2.0);
    CHECK_CTX(percentile(s, 0.50) > 0.0, "latency p50 came back as zero");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. batchDequeue(N) vs N x dequeue(), single consumer (the method is
//    MPSC-only by contract — it stages into one shared array).
//
// Both sides drain the same item count from the same buffer type with one
// producer feeding, so the difference is purely the per-item ticket round
// trip that batching amortizes.
// ─────────────────────────────────────────────────────────────────────────────
#if !ASTRA_SKIP_BATCH_DEQUEUE_TESTS
static const std::size_t BATCH = 50;

template <bool UseBatch>
static double runDrainOnce(uint64_t total) {
    RB::AtomicRingBuffer<uint64_t, 2048, BATCH> q;
    std::atomic<bool> go{false};
    std::thread producer([&] {
        pinToCore(0);
        while (!go.load(std::memory_order_acquire)) _mm_pause();
        for (uint64_t i = 0; i < total; ++i) q.enqueue(i);
        g_produced.store(total, std::memory_order_relaxed);
    });

    pinToCore(1);
    const int64_t t0 = nowNs();
    go.store(true, std::memory_order_release);
    uint64_t sum = 0, taken = 0;
    while (taken < total) {
        if constexpr (UseBatch) {
            const uint64_t want = std::min<uint64_t>(BATCH, total - taken);
            auto* arr = q.batchDequeue(int(want));
            for (uint64_t i = 0; i < want; ++i) sum += (*arr)[i];
            taken += want;
        } else {
            sum += q.dequeue();
            ++taken;
        }
        doNotOptimize(sum);
    }
    const double elapsedNs = double(nowNs() - t0);
    g_consumed.store(taken, std::memory_order_relaxed);
    producer.join();

    CHECK_CTX(sum == total * (total - 1) / 2,
              (UseBatch ? "batchDequeue" : "dequeue") << " drain: value sum "
              << sum << " != expected " << (total * (total - 1) / 2));
    return elapsedNs;
}

static void benchBatch(uint64_t total) {
    Deadline dl("batch_vs_single", 120);
    std::vector<double> single, batch;
    for (int i = 0; i < WARMUP; ++i) { runDrainOnce<false>(total); runDrainOnce<true>(total); }
    for (int i = 0; i < g_reps; ++i) single.push_back(runDrainOnce<false>(total));
    for (int i = 0; i < g_reps; ++i) batch.push_back(runDrainOnce<true>(total));

    reportThroughput("drain_single", "1P/1C", total, single);
    reportThroughput("drain_batch50", "1P/1C", total, batch);
    const double sNs = median(single) / double(total);
    const double bNs = median(batch)  / double(total);
    std::printf("  %-22s %-14s batching is %.2fx %s per item (%.2f -> %.2f ns)\n",
                "", "", (bNs > 0 ? sNs / bNs : 0.0),
                (bNs < sNs ? "faster" : "SLOWER"), sNs, bNs);
}

#endif  // !ASTRA_SKIP_BATCH_DEQUEUE_TESTS

// ─────────────────────────────────────────────────────────────────────────────
// 5. Payload size. The copy/move into the slot happens after the ticket is
//    claimed but before seq is published, so a larger payload holds the slot
//    longer and directly widens the window a waiting thread spins in.
//
//    120 bytes is the largest Slot's static_assert allows (128 - sizeof(seq)),
//    which makes sizeof(Slot) 128 — two cache lines per slot rather than one.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
static double runPayloadOnce(uint64_t total) {
    RB::AtomicRingBuffer<T, 2048> q;
    std::atomic<bool> go{false};
    std::thread producer([&] {
        pinToCore(0);
        while (!go.load(std::memory_order_acquire)) _mm_pause();
        T v{};
        for (uint64_t i = 0; i < total; ++i) q.enqueue(v);
        g_produced.store(total, std::memory_order_relaxed);
    });

    pinToCore(1);
    const int64_t t0 = nowNs();
    go.store(true, std::memory_order_release);
    for (uint64_t i = 0; i < total; ++i) {
        T v = q.dequeue();
        doNotOptimize(v);
    }
    const double elapsedNs = double(nowNs() - t0);
    g_consumed.store(total, std::memory_order_relaxed);
    producer.join();
    return elapsedNs;
}

template <typename T>
static void benchPayload(const char* label, uint64_t total) {
    Deadline dl("payload_size", 120);
    for (int i = 0; i < WARMUP; ++i) runPayloadOnce<T>(total);
    std::vector<double> runs;
    for (int i = 0; i < g_reps; ++i) runs.push_back(runPayloadOnce<T>(total));
    char config[32];
    std::snprintf(config, sizeof(config), "%zuB/slot %zuB",
                  sizeof(T), sizeof(RB::Slot<T>));
    reportThroughput(label, config, total, runs);
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    if (const char* s = std::getenv("ASTRA_PERF_SCALE")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v > 1) g_scale = uint64_t(v);
    }
    if (const char* s2 = std::getenv("ASTRA_PERF_MULT")) {
        const long v = std::strtol(s2, nullptr, 10);
        if (v > 1) g_mult = uint64_t(v);
    }
    if (const char* s2 = std::getenv("ASTRA_PERF_REPS")) {
        const long v = std::strtol(s2, nullptr, 10);
        if (v >= 1) g_reps = int(v);
    }
    if (const char* v = std::getenv("ASTRA_PERF_VERBOSE")) g_verbose = (v[0] != '0');
    detectTopology();
    const bool doProbe = !std::getenv("ASTRA_PERF_NOPROBE");
    if (doProbe) probeAndOrderCores();
    detectInvariantTsc();
    calibrateTsc();

    std::thread(watchdogLoop).detach();

    // One Timer for the whole process: its constructor calibrates GHz with a
    // blocking sleep(1), so building more than one is pure wasted wall time.
    //
    // Cross-check it against steady_clock over the same interval before
    // trusting anything below. Timer derives ns from its own GHz calibration,
    // and if that calibration is off every Timer-derived figure here is off
    // by the same factor — silently, and in a way no amount of repetition
    // would reveal.
    AstraLib::Time::Timer timer;
    const int64_t xcheckT0 = nowNs();
    timer.start();
    const uint64_t spin = 50'000'000;
    for (uint64_t i = 0; i < spin; ++i) doNotOptimize(i);
    const double timerNs = timer.getTimeNs();
    const double steadyNs = double(nowNs() - xcheckT0);

    std::printf("AtomicRingBuffer performance characterization\n");
    std::printf("  cores: %u physical / %u logical (pinning one thread per physical core)\n",
                coreCount(), std::thread::hardware_concurrency());
    if (doProbe && g_bestPairCycles) {
        const double ratio = double(g_worstPairCycles) / double(g_bestPairCycles);
        std::printf("  pairwise handoff probe: best pair %.0f cycles (%.0f ns), "
                    "worst %.0f cycles (%.0f ns), spread %.2fx%s\n",
                    double(g_bestPairCycles), cyclesToNs(g_bestPairCycles),
                    double(g_worstPairCycles), cyclesToNs(g_worstPairCycles), ratio,
                    ratio > 1.5 ? "  << NON-UNIFORM cache topology" : "");
        std::printf("  core order (%s):",
                    g_probeReordered ? "reordered cheapest-talking first"
                                     : "sysfs order kept; spread too small to act on");
        for (unsigned c : g_physCpus) std::printf(" cpu%u", c);
        std::printf("\n");
    }
    std::printf("  TSC: %.3f GHz (invariant: %s)   items: x%llu/%llu\n",
                g_tscGhz, g_invariantTsc ? "yes" : "NO",
                (unsigned long long)g_mult, (unsigned long long)g_scale);
    const double xcheckRatio = timerNs / steadyNs;
    std::printf("  clock cross-check: Timer %.2f ms vs steady_clock %.2f ms "
                "(ratio %.4f)\n", timerNs / 1e6, steadyNs / 1e6, xcheckRatio);
    CHECK_CTX(xcheckRatio > 0.90 && xcheckRatio < 1.10,
              "Timer disagrees with steady_clock by " << (xcheckRatio - 1.0) * 100.0
              << "% over the same interval — its GHz calibration is wrong, so "
              "every Timer-derived figure below is scaled by that same factor");
    std::printf("  median of %d runs after %d discarded warmup run(s)\n\n", g_reps, WARMUP);

    // Every benchmark here needs at least a producer and a consumer on
    // separate physical cores. Below that there is nothing to measure: the
    // two would share a core and spin against each other, which is the
    // pathology documented in docs/tests.md, not a queue measurement. Note
    // a 2-vCPU CI host is typically ONE physical core with SMT, so this does
    // fire there — which is the other reason this binary is not in the
    // default ctest run.
    if (coreCount() < 2) {
        std::printf("SKIPPED: %u physical core(s). Benchmarking a spin-based "
                    "queue needs at least 2.\n", coreCount());
        return 0;
    }

    std::printf("[1] SPSC ceiling\n");
    benchThroughput<2048>("spsc_throughput", 1, 1, scaled(2'000'000));

    std::printf("\n[2] MPMC scaling (fixed total work)\n");
    benchThroughput<2048>("mpmc_scaling", 1, 1, scaled(2'000'000));
    benchThroughput<2048>("mpmc_scaling", 2, 2, scaled(2'000'000));
    benchThroughput<2048>("mpmc_scaling", 4, 4, scaled(2'000'000));

    std::printf("\n[3] Handoff latency\n");
    benchLatency(scaled(200'000, 20'000));

#if ASTRA_SKIP_BATCH_DEQUEUE_TESTS
    std::printf("\n[4] batchDequeue vs dequeue  -- SKIPPED (batchDequeue under rework)\n");
#else
    std::printf("\n[4] batchDequeue vs dequeue\n");
    benchBatch(scaled(2'000'000));
#endif

    std::printf("\n[5] Payload size\n");
    benchPayload<uint32_t>  ("payload_4B",   scaled(2'000'000));
    benchPayload<Payload56> ("payload_56B",  scaled(2'000'000));
    benchPayload<Payload120>("payload_120B", scaled(2'000'000));

    // Re-probe the same core the run started on. TSC is invariant, so more
    // cycles for identical work means the core is now running slower than it
    // was at startup — sustained all-core load on this box costs roughly 2x
    // and keeps climbing, which biases later reps downward against earlier
    // ones. Reported rather than corrected for: there is no honest way to
    // rescale results after the fact, but a reader deserves to know the
    // measurement conditions drifted under them.
    if (doProbe && !g_probe.empty()) {
        const uint64_t after = probeCpu(g_probe.front().cpu, 3);
        const double drift = double(after) / double(g_probeBaselineCycles);
        std::printf("\n  thermal/frequency drift on cpu%u: %.1f -> %.1f Mcycles "
                    "for identical work (%.2fx)%s\n",
                    g_probe.front().cpu, double(g_probeBaselineCycles) / 1e6,
                    double(after) / 1e6, drift,
                    drift > 1.25 ? "  << the machine slowed down DURING this run;"
                                   " later rows are biased low" : "");
    }

    const long f = g_failures.load();
    if (f == 0) {
        std::printf("\nAll accounting checks passed. Numbers above are reported, not gated.\n");
        return 0;
    }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
