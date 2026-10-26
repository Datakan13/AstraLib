// ─────────────────────────────────────────────────────────────────────────────
// AstraLib::Pools::ThreadSafeIndexPool vs other ways to hand out free slots.
//
// An index pool is a concurrent free list: getIndex() claims an unused index,
// returnIndex() gives it back. Competitors, each pre-filled with 0..SIZE-1:
//
//   AstraLib ThreadSafeIndexPool  AtomicRingBuffer<int> underneath (FIFO)
//   rigtorp::MPMCQueue<int>       same ring design, used as a free list (FIFO)
//   boost::lockfree::stack<int>   Treiber stack, fixed capacity (LIFO)
//   moodycamel::ConcurrentQueue   per-producer sub-queues (no global order)
//   std::mutex + std::vector      the baseline free list (LIFO)
//
// FIFO vs LIFO is a real behavioural difference, not just an implementation
// detail: a LIFO free list hands a thread back the index it just returned,
// whose data is still hot in that core's cache. FIFO cycles through all SIZE
// indices. The workload below touches the claimed index's data, so that
// locality effect is included in the numbers — on purpose, since it's what a
// real caller would see.
//
// Workload: each thread loops get → write to data[idx] → return, a fixed
// number of times. Throughput is get+return PAIRS per second.
//
// Accounting: data[idx] is a plain (non-atomic) counter, so it is only exact
// if no index was ever held by two threads at once. Its sum must equal the
// total number of pairs; a duplicated index loses increments and shows up as
// a shortfall.
// ─────────────────────────────────────────────────────────────────────────────
#include "benchCommon.hpp"

#include <AstraLib/Pools/threadSafeIndexPool.hpp>

#include <boost/lockfree/stack.hpp>
#include <concurrentqueue.h>
#include <rigtorp/MPMCQueue.h>

#include <memory>
#include <mutex>

using namespace Bench;

static constexpr std::size_t SIZE = 1024;

// ── adapters: int get(), void put(int) ──────────────────────────────────────
struct AstraPool {
    AstraLib::Pools::ThreadSafeIndexPool<SIZE> p;
    int get() { return p.getIndex(); }
    void put(int i) { p.returnIndex(i); }
};

struct RigtorpPool {
    rigtorp::MPMCQueue<int> q{SIZE};
    RigtorpPool() { for (int i = 0; i < int(SIZE); ++i) q.push(i); }
    int get() { int v; q.pop(v); return v; }
    void put(int i) { q.push(i); }
};

struct BoostStackPool {
    boost::lockfree::stack<int, boost::lockfree::capacity<SIZE>> s;
    BoostStackPool() { for (int i = 0; i < int(SIZE); ++i) s.push(i); }
    int get() { int v; while (!s.pop(v)) _mm_pause(); return v; }
    void put(int i) { while (!s.push(i)) _mm_pause(); }
};

struct MoodyPool {
    moodycamel::ConcurrentQueue<int> q{SIZE};
    MoodyPool() { for (int i = 0; i < int(SIZE); ++i) q.enqueue(i); }
    int get() { int v; while (!q.try_dequeue(v)) _mm_pause(); return v; }
    void put(int i) { q.enqueue(i); }
};

struct MutexVecPool {
    std::mutex m;
    std::vector<int> free;
    MutexVecPool() { for (int i = 0; i < int(SIZE); ++i) free.push_back(i); }
    int get() {
        for (;;) {
            {
                std::lock_guard<std::mutex> g(m);
                if (!free.empty()) { int v = free.back(); free.pop_back(); return v; }
            }
            _mm_pause();
        }
    }
    void put(int i) {
        std::lock_guard<std::mutex> g(m);
        free.push_back(i);
    }
};

// ── measurement ─────────────────────────────────────────────────────────────
struct alignas(64) Slot { uint64_t uses = 0; };

template <class P>
static double run(unsigned threads, uint64_t total, const char* name) {
    auto pool = std::make_unique<P>();
    auto data = std::make_unique<Slot[]>(SIZE);
    const uint64_t perThread = total / threads;

    const int64_t ns = runPinned(threads, [&](unsigned) {
        for (uint64_t k = 0; k < perThread; ++k) {
            const int idx = pool->get();
            ++data[idx].uses;
            pool->put(idx);
        }
    });

    uint64_t sum = 0;
    for (std::size_t i = 0; i < SIZE; ++i) sum += data[i].uses;
    BENCH_CHECK(sum == perThread * threads,
                "%s x%u: %llu recorded uses vs %llu pairs — an index was handed out twice",
                name, threads, (unsigned long long)sum,
                (unsigned long long)(perThread * threads));
    return double(perThread * threads) * 1e3 / double(ns);   // M pairs/s
}

struct Competitor {
    const char* name;
    const char* note;
    double (*fn)(unsigned, uint64_t, const char*);
};

int main() {
    init();
    printPreamble("ThreadSafeIndexPool vs other free lists  (pool size 1024)");

    const std::vector<Competitor> all = {
        {"AstraLib ThreadSafeIndexPool", "(FIFO)",      &run<AstraPool>},
        {"rigtorp::MPMCQueue<int>",      "(FIFO)",      &run<RigtorpPool>},
        {"boost::lockfree::stack<int>",  "(LIFO)",      &run<BoostStackPool>},
        {"moodycamel::ConcurrentQueue",  "(unordered)", &run<MoodyPool>},
        {"std::mutex + std::vector",     "(LIFO)",      &run<MutexVecPool>},
    };

    std::printf("\n[1] get+return throughput, M pairs/s (higher is better)\n");
    const uint64_t total = scaled(4'000'000);
    for (unsigned t : {1u, 2u, 4u, 8u}) {
        char title[48];
        std::snprintf(title, sizeof(title), "%u thread%s%s", t, t == 1 ? "" : "s",
                      t == 1 ? " (uncontended)" : "");
        if (t + 1 > coreCount()) {
            std::printf("\n  %s  SKIPPED — needs %u physical cores\n", title, t + 1);
            continue;
        }
        std::vector<std::function<double()>> fns;
        std::vector<std::string> names;
        for (const Competitor& c : all) {
            names.push_back(c.name);
            fns.push_back([&c, t, total] { return c.fn(t, total, c.name); });
        }
        Deadline dl(title, 600);
        const std::vector<Stat> st = interleaved(fns, names);
        std::vector<Row> rows;
        for (size_t i = 0; i < all.size(); ++i) rows.push_back({all[i].name, all[i].note, st[i]});
        printTable(title, "M pair/s", rows);
    }
    return finish();
}
