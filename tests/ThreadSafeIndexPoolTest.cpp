// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Pools::ThreadSafeIndexPool.
//
// This is a resource-checkout pool built on AtomicRingBuffer<int, SIZE>. Its
// one correctness property is the one that matters for every caller: an
// index handed out by getIndex() must be held EXCLUSIVELY until the holder
// calls returnIndex() — no two threads may simultaneously believe they own
// the same index. A per-index "in use" flag (CAS-style fetch_add/fetch_sub)
// makes any double-checkout show up immediately rather than as a rare,
// hard-to-reproduce corruption downstream.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Pools/threadSafeIndexPool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

static std::atomic<long> g_failures{0};

#define CHECK_CTX(cond, ctx)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failures.fetch_add(1, std::memory_order_relaxed);              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__            \
                      << "  CHECK(" #cond ")  " << ctx << "\n";              \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_CTX(cond, "")

static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

using AstraLib::Pools::ThreadSafeIndexPool;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Fresh pool contains exactly {0, ..., SIZE-1}, no duplicates, nothing
//    out of range.
// ─────────────────────────────────────────────────────────────────────────────
static void test_initial_fill_is_unique_and_complete() {
    constexpr int SIZE = 32;
    ThreadSafeIndexPool<SIZE> pool;
    std::set<int> seen;
    for (int i = 0; i < SIZE; ++i) {
        int idx = pool.getIndex();
        CHECK_CTX(idx >= 0 && idx < SIZE, "index " << idx << " out of range [0," << SIZE << ")");
        bool inserted = seen.insert(idx).second;
        CHECK_CTX(inserted, "duplicate index " << idx << " in initial fill");
    }
    CHECK_CTX(seen.size() == SIZE, "expected " << SIZE << " unique indices, got " << seen.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. No double-checkout under heavy concurrency. Many threads repeatedly
//    check an index out, do a little "work" while holding it (to widen the
//    window a bug could exploit), and return it. A per-index atomic flag
//    catches the instant two threads both believe they hold the same index.
// ─────────────────────────────────────────────────────────────────────────────
static void test_no_double_checkout_under_contention() {
    constexpr int SIZE = 16;
    constexpr int THREADS = 32;
    constexpr int ROUNDS = 5000;
    ThreadSafeIndexPool<SIZE> pool;
    std::vector<std::atomic<int>> inUse(SIZE);
    for (auto& f : inUse) f.store(0, std::memory_order_relaxed);
    std::atomic<long> doubleCheckouts{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&] {
            for (int r = 0; r < ROUNDS; ++r) {
                int idx = pool.getIndex();
                int prev = inUse[idx].fetch_add(1, std::memory_order_acq_rel);
                if (prev != 0) doubleCheckouts.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();   // widen the exclusive-hold window
                inUse[idx].fetch_sub(1, std::memory_order_acq_rel);
                pool.returnIndex(idx);
            }
        });
    for (auto& th : ts) th.join();

    CHECK_CTX(doubleCheckouts.load() == 0,
              doubleCheckouts.load() << " double-checkout(s): two threads held the "
              "same index simultaneously");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Exhaustion + backpressure: more threads than indices (SIZE=4, 12
//    threads) forces getIndex() to actually block callers until a holder
//    returns. Correctness under this pressure is the same double-checkout
//    check, just guaranteed to hit contention every round instead of maybe.
// ─────────────────────────────────────────────────────────────────────────────
static void test_exhaustion_forces_backpressure_not_corruption() {
    constexpr int SIZE = 4;
    constexpr int THREADS = 12;
    constexpr int ROUNDS = 3000;
    ThreadSafeIndexPool<SIZE> pool;
    std::vector<std::atomic<int>> inUse(SIZE);
    for (auto& f : inUse) f.store(0, std::memory_order_relaxed);
    std::atomic<long> doubleCheckouts{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t)
        ts.emplace_back([&] {
            for (int r = 0; r < ROUNDS; ++r) {
                int idx = pool.getIndex();
                CHECK_CTX(idx >= 0 && idx < SIZE, "index " << idx << " out of range");
                int prev = inUse[idx].fetch_add(1, std::memory_order_acq_rel);
                if (prev != 0) doubleCheckouts.fetch_add(1, std::memory_order_relaxed);
                inUse[idx].fetch_sub(1, std::memory_order_acq_rel);
                pool.returnIndex(idx);
            }
        });
    for (auto& th : ts) th.join();

    CHECK_CTX(doubleCheckouts.load() == 0,
              doubleCheckouts.load() << " double-checkout(s) under 3x-oversubscribed contention");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. reInitializePool(): quiescent reset (all indices returned first, per the
//    same quiescent-only contract clearBuffer() documents) must yield a
//    fresh, complete, duplicate-free {0,...,SIZE-1} again — and the pool
//    must remain fully functional afterward.
// ─────────────────────────────────────────────────────────────────────────────
static void test_reinitialize_restores_full_unique_set() {
    constexpr int SIZE = 8;   // AtomicRingBuffer requires a power-of-two size
    ThreadSafeIndexPool<SIZE> pool;

    // Churn it a bit, then return everything so the pool is quiescent.
    std::vector<int> held;
    for (int i = 0; i < SIZE; ++i) held.push_back(pool.getIndex());
    for (int idx : held) pool.returnIndex(idx);

    pool.reInitializePool();

    std::set<int> seen;
    for (int i = 0; i < SIZE; ++i) {
        int idx = pool.getIndex();
        CHECK_CTX(idx >= 0 && idx < SIZE, "post-reinit index " << idx << " out of range");
        CHECK_CTX(seen.insert(idx).second, "post-reinit duplicate index " << idx);
    }
    CHECK_CTX(seen.size() == SIZE, "expected " << SIZE << " unique indices after reinit, got " << seen.size());

    // Pool must still work normally afterward: return one, get it back.
    pool.returnIndex(*seen.begin());
    int idx = pool.getIndex();
    CHECK_CTX(idx == *seen.begin(), "pool not functional after reinit: expected " << *seen.begin() << " got " << idx);
}

int main() {
    std::cout << "ThreadSafeIndexPool test suite  (hw threads: "
              << std::thread::hardware_concurrency() << ")\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"initial_fill_is_unique_and_complete",         test_initial_fill_is_unique_and_complete},
        {"no_double_checkout_under_contention",         test_no_double_checkout_under_contention},
        {"exhaustion_forces_backpressure_not_corruption", test_exhaustion_forces_backpressure_not_corruption},
        {"reinitialize_restores_full_unique_set",       test_reinitialize_restores_full_unique_set},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        int64_t t0 = nowNs();
        long before = g_failures.load();
        t.fn();
        double ms = double(nowNs() - t0) / 1e6;
        std::cout << (g_failures.load() == before ? "[  OK  ] " : "[ BAD  ] ")
                  << t.name << "  (" << ms << " ms)" << std::endl;
    }

    long f = g_failures.load();
    if (f == 0) { std::cout << "\nALL THREADSAFEINDEXPOOL TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
