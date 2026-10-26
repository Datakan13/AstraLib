// ─────────────────────────────────────────────────────────────────────────────
// AstraLib::Threading::ThreadPool vs other thread pools.
//
// Competitors:
//   AstraLib ThreadPool<W>   W workers + 1 dispatcher thread; lock-free ring
//                            buffer task queue, futex sleep/wake per worker
//   BS::thread_pool          bshoshany/thread-pool v5 — mutex + condvar
//                            queue, the most widely used header-only pool
//   condvar pool             the textbook std::mutex + std::condition_variable
//                            + std::queue pool, written inline as a baseline
//
// All three take std::function<void()>-style tasks, so the per-task type
// erasure cost is the same for everyone.
//
// Sections:
//   [1] burst throughput: submit N tasks from one thread as fast as possible,
//       time until the last one has RUN. Two task sizes:
//         empty  — pure dispatch overhead
//         ~1 us  — a realistic small task; overhead should start to vanish
//   [2] wake latency: with the pool idle (workers asleep), submit ONE task and
//       measure submit → task-start. This is the cost a latency-sensitive
//       caller actually pays, and where futex vs condvar differ.
//
// Completion is detected the same way for every pool — each task bumps an
// atomic counter and the submitter spins until it reaches N — because the
// AstraLib pool has no wait()/join API. BS::thread_pool::wait() is
// deliberately NOT used so it gets no different treatment.
//
// Placement caveat: the SUBMITTER is pinned to its own physical core, but pool
// worker threads are created inside each library and are left to the
// scheduler (none of them expose an affinity hook). Expect more run-to-run
// variance here than in the queue/lock benchmarks.
// ─────────────────────────────────────────────────────────────────────────────
#include "benchCommon.hpp"

#include <AstraLib/Threading/ThreadPool.hpp>

#include <BS_thread_pool.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

using namespace Bench;

// ── the baseline everyone writes first ──────────────────────────────────────
class CondvarPool {
    std::mutex m;
    std::condition_variable cv;
    std::queue<std::function<void()>> q;
    std::vector<std::thread> workers;
    bool stop = false;

public:
    explicit CondvarPool(unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cv.wait(lk, [this] { return stop || !q.empty(); });
                        if (stop && q.empty()) return;
                        task = std::move(q.front());
                        q.pop();
                    }
                    task();
                }
            });
    }
    void submit(std::function<void()>&& f) {
        {
            std::lock_guard<std::mutex> g(m);
            q.push(std::move(f));
        }
        cv.notify_one();
    }
    ~CondvarPool() {
        {
            std::lock_guard<std::mutex> g(m);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : workers) t.join();
    }
};

// ── adapters: construct with W workers, submit(std::function&&) ─────────────
template <unsigned W>
struct AstraPool {
    AstraLib::Threading::ThreadPool<W> p;
    void submit(std::function<void()>&& f) { p.assignTask(std::move(f)); }
};

template <unsigned W>
struct BSPool {
    BS::thread_pool<> p{W};
    void submit(std::function<void()>&& f) { p.detach_task(std::move(f)); }
};

template <unsigned W>
struct CVPool {
    CondvarPool p{W};
    void submit(std::function<void()>&& f) { p.submit(std::move(f)); }
};

// ── workload ────────────────────────────────────────────────────────────────
static uint64_t g_taskCycles = 0;   // ~1 us worth of TSC ticks, set in main

static inline void spinCycles(uint64_t c) {
    const uint64_t end = __rdtsc() + c;
    while (__rdtsc() < end) {}
}

template <class P>
static double burst(uint64_t N, bool heavy, const char* name) {
    auto pool = std::make_unique<P>();
    std::atomic<uint64_t> done{0};
    const uint64_t cyc = heavy ? g_taskCycles : 0;

    pinToCore(0);
    const int64_t t0 = nowNs();
    for (uint64_t i = 0; i < N; ++i)
        pool->submit([&done, cyc] {
            if (cyc) spinCycles(cyc);
            done.fetch_add(1, std::memory_order_release);
        });
    while (done.load(std::memory_order_acquire) < N) _mm_pause();
    const int64_t t1 = nowNs();
    unpin();

    BENCH_CHECK(done.load() == N, "%s: %llu of %llu tasks ran", name,
                (unsigned long long)done.load(), (unsigned long long)N);
    pool.reset();   // destruction (thread joins) is outside the timed window
    return double(N) * 1e3 / double(t1 - t0);   // M tasks/s
}

template <class P>
static std::vector<double> wakeLatency(uint64_t samples) {
    auto pool = std::make_unique<P>();
    std::vector<double> out;
    out.reserve(samples);
    pinToCore(0);
    for (uint64_t k = 0; k < samples; ++k) {
        // Let every worker go back to sleep so each sample is a cold wake.
        spinCycles(uint64_t(200'000 * g_tscPerNs));   // 200 us
        std::atomic<uint64_t> started{0};
        const uint64_t t0 = __rdtsc();
        pool->submit([&started] { started.store(__rdtsc(), std::memory_order_release); });
        uint64_t s;
        while ((s = started.load(std::memory_order_acquire)) == 0) _mm_pause();
        out.push_back(cyclesToNs(s - t0));
    }
    unpin();
    std::sort(out.begin(), out.end());
    return out;
}

struct Competitor {
    const char* name;
    double (*burst)(uint64_t, bool, const char*);
    std::vector<double> (*wake)(uint64_t);
};

template <unsigned W>
static std::vector<Competitor> competitors() {
    return {
        {"AstraLib ThreadPool", &burst<AstraPool<W>>, &wakeLatency<AstraPool<W>>},
        {"BS::thread_pool",     &burst<BSPool<W>>,    &wakeLatency<BSPool<W>>},
        {"condvar pool",        &burst<CVPool<W>>,    &wakeLatency<CVPool<W>>},
    };
}

static void runWorkers(unsigned W, const std::vector<Competitor>& all) {
    // Submitter + W workers (+ AstraLib's dispatcher) must fit without
    // oversubscription — AstraLib's dispatcher spins while hunting a free worker.
    if (W + 2 > coreCount()) {
        std::printf("\n  %u workers  SKIPPED — needs %u physical cores\n", W, W + 2);
        return;
    }
    for (bool heavy : {false, true}) {
        char title[64];
        std::snprintf(title, sizeof(title), "%u workers, %s tasks", W,
                      heavy ? "~1 us" : "empty");
        const uint64_t N = scaled(heavy ? 100'000 : 200'000);
        std::vector<std::function<double()>> fns;
        std::vector<std::string> names;
        for (const Competitor& c : all) {
            names.push_back(c.name);
            fns.push_back([&c, N, heavy] { return c.burst(N, heavy, c.name); });
        }
        Deadline dl(title, 600);
        const std::vector<Stat> st = interleaved(fns, names);
        std::vector<Row> rows;
        for (size_t i = 0; i < all.size(); ++i) rows.push_back({all[i].name, "", st[i]});
        printTable(title, "M task/s", rows);
    }
}

int main() {
    init();
    g_taskCycles = uint64_t(1000 * g_tscPerNs);
    printPreamble("ThreadPool vs other thread pools  (submitter pinned; pool threads unpinned)");

    std::printf("\n[1] Burst throughput, M tasks/s (higher is better)\n");
    runWorkers(2, competitors<2>());
    runWorkers(4, competitors<4>());
    runWorkers(8, competitors<8>());

    std::printf("\n[2] Wake latency, idle pool → task start in ns (lower is better), 4 workers\n");
    if (4 + 2 > coreCount()) {
        std::printf("  SKIPPED — needs 6 physical cores\n");
    } else {
        const uint64_t samples = scaled(2000, 200);
        std::printf("    %-30s %9s %9s %9s %10s\n", "", "p50", "p90", "p99", "max");
        for (const Competitor& c : competitors<4>()) {
            Deadline dl(c.name, 300);
            c.wake(samples / 10);   // warmup
            std::vector<double> r = c.wake(samples);
            std::printf("    %-30s %9.0f %9.0f %9.0f %10.0f\n", c.name,
                        percentile(r, 0.50), percentile(r, 0.90),
                        percentile(r, 0.99), r.back());
        }
    }
    return finish();
}
