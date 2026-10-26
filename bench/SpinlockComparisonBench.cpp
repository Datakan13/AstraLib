// ─────────────────────────────────────────────────────────────────────────────
// AstraLib::Atomic::Spinlock vs other locks.
//
// Competitors:
//   AstraLib Spinlock            default construction (waitTime = 1000 pauses
//                                of backoff after each failed exchange)
//   AstraLib Spinlock(wait=16)   same lock, short backoff — isolates how much
//                                of the result is the backoff constant rather
//                                than the algorithm
//   TTAS spinlock                test-and-test-and-set with _mm_pause: spin on
//                                a plain load and only attempt the exchange
//                                when the lock looks free (the design in
//                                rigtorp.se/spinlock, written inline here —
//                                it's 15 lines, not a library)
//   pthread_spin_lock            glibc's spinlock
//   std::mutex                   futex-backed, sleeps under contention
//
// Each run is FIXED DURATION: every thread loops lock → critical section →
// unlock → small outside-the-lock gap until its time is up, counting
// acquisitions. That gives two numbers per row:
//   throughput  total acquisitions / wall time
//   fairness    min/max per-thread acquisition count (1.00 = perfectly even;
//               near 0 = one thread hogs the lock).
//   handoffs    times per second the lock passed to a DIFFERENT thread. A
//               lock can post high throughput by letting the current holder
//               re-acquire over and over while waiters back off — the
//               protected data never leaves that core's cache, so each
//               acquisition is nearly uncontended. Throughput with a low
//               handoff rate means "fast for whoever holds it", not "fast
//               under contention"; read the two together.
//
// Accounting: the critical section increments a plain (non-atomic) counter.
// If any lock admitted two threads at once, increments would be lost and the
// counter would fall short of the summed acquisition counts.
// ─────────────────────────────────────────────────────────────────────────────
#include "benchCommon.hpp"

#include <AstraLib/Atomic/spinlock.hpp>

#include <memory>
#include <mutex>

using namespace Bench;

// ── lock adapters ───────────────────────────────────────────────────────────
struct AstraDefault {
    AstraLib::Atomic::Spinlock l;
    void lock() { l.lock(); }
    void unlock() { l.unlock(); }
};

struct AstraShortWait {
    AstraLib::Atomic::Spinlock l{16};
    void lock() { l.lock(); }
    void unlock() { l.unlock(); }
};

struct alignas(64) TTAS {
    std::atomic<bool> f{false};
    void lock() {
        for (;;) {
            if (!f.exchange(true, std::memory_order_acquire)) return;
            while (f.load(std::memory_order_relaxed)) _mm_pause();
        }
    }
    void unlock() { f.store(false, std::memory_order_release); }
};

struct alignas(64) PthreadSpin {
    pthread_spinlock_t s;
    PthreadSpin() { pthread_spin_init(&s, PTHREAD_PROCESS_PRIVATE); }
    ~PthreadSpin() { pthread_spin_destroy(&s); }
    void lock() { pthread_spin_lock(&s); }
    void unlock() { pthread_spin_unlock(&s); }
};

struct alignas(64) StdMutex {
    std::mutex m;
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
};

// ── workload ────────────────────────────────────────────────────────────────
static inline void work(unsigned iters) {
    uint64_t x = 0;
    for (unsigned i = 0; i < iters; ++i) { x += i; doNotOptimize(x); }
}

struct alignas(64) Shared { uint64_t counter = 0; uint64_t handoffs = 0; unsigned owner = ~0u; };
struct alignas(64) PerThread { uint64_t count = 0; };

struct Sample { double mops; double fairness; double handoffsM; };

template <class L>
static Sample runLock(unsigned threads, unsigned csIters, int64_t durNs, const char* name) {
    auto lock = std::make_unique<L>();
    auto shared = std::make_unique<Shared>();
    std::vector<PerThread> per(threads);
    const uint64_t durCycles = uint64_t(double(durNs) * g_tscPerNs);

    const int64_t ns = runPinned(threads, [&](unsigned i) {
        const uint64_t end = __rdtsc() + durCycles;
        uint64_t n = 0;
        while (__rdtsc() < end) {
            lock->lock();
            ++shared->counter;
            if (shared->owner != i) { shared->owner = i; ++shared->handoffs; }
            work(csIters);
            lock->unlock();
            ++n;
            work(16);   // a little life outside the lock, as real callers have
        }
        per[i].count = n;
    });

    uint64_t total = 0, lo = UINT64_MAX, hi = 0;
    for (auto& p : per) {
        total += p.count;
        lo = std::min(lo, p.count);
        hi = std::max(hi, p.count);
    }
    BENCH_CHECK(shared->counter == total,
                "%s x%u: counter %llu vs %llu acquisitions — mutual exclusion broken",
                name, threads, (unsigned long long)shared->counter,
                (unsigned long long)total);
    return {double(total) * 1e3 / double(ns), hi ? double(lo) / double(hi) : 0,
            double(shared->handoffs) * 1e3 / double(ns)};
}

struct Competitor {
    const char* name;
    Sample (*fn)(unsigned, unsigned, int64_t, const char*);
};

int main() {
    init();
    printPreamble("Spinlock vs other locks  (fixed-duration runs, one thread per physical core)");

    const std::vector<Competitor> all = {
        {"AstraLib Spinlock",          &runLock<AstraDefault>},
        {"AstraLib Spinlock(wait=16)", &runLock<AstraShortWait>},
        {"TTAS spinlock",              &runLock<TTAS>},
        {"pthread_spin_lock",          &runLock<PthreadSpin>},
        {"std::mutex",                 &runLock<StdMutex>},
    };

    const int64_t durNs = int64_t(scaled(200, 10)) * 1'000'000;   // 200 ms
    struct CS { const char* label; unsigned iters; };
    const CS sections[] = {{"tiny critical section (counter++)", 0},
                           {"~100-iteration critical section", 100}};
    const unsigned threadCounts[] = {1, 2, 4, 8};

    for (const CS& cs : sections) {
        std::printf("\n[%s]  throughput in M acquisitions/s (higher is better), "
                    "fairness = min/max per-thread share, handoffs = owner changes/s\n", cs.label);
        for (unsigned t : threadCounts) {
            char title[64];
            std::snprintf(title, sizeof(title), "%u thread%s%s", t, t == 1 ? "" : "s",
                          t == 1 ? " (uncontended)" : "");
            if (t + 1 > coreCount()) {
                std::printf("\n  %s  SKIPPED — needs %u physical cores\n", title, t + 1);
                continue;
            }
            std::vector<std::vector<double>> fair(all.size()), hand(all.size());
            std::vector<std::function<double()>> fns;
            std::vector<std::string> names;
            for (size_t k = 0; k < all.size(); ++k) {
                names.push_back(all[k].name);
                fns.push_back([&, k, t] {
                    Sample s = all[k].fn(t, cs.iters, durNs, all[k].name);
                    fair[k].push_back(s.fairness);
                    hand[k].push_back(s.handoffsM);
                    return s.mops;
                });
            }
            Deadline dl(title, 600);
            const std::vector<Stat> st = interleaved(fns, names);
            std::vector<Row> rows;
            for (size_t k = 0; k < all.size(); ++k) {
                char note[64] = "";
                if (t > 1) {
                    // drop the warmup sample, matching the throughput stats
                    std::vector<double> f(fair[k].begin() + 1, fair[k].end());
                    std::vector<double> h(hand[k].begin() + 1, hand[k].end());
                    std::snprintf(note, sizeof(note), "fairness %.2f  handoffs %5.2f M/s",
                                  summarize(f).median, summarize(h).median);
                }
                rows.push_back({all[k].name, note, st[k]});
            }
            printTable(title, "M acq/s", rows);
        }
    }
    return finish();
}
