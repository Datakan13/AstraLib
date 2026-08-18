// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Threading::ThreadPool.
//
// TWO SEVERE BUGS confirmed here, both via isolated repros (outside this
// file, with a hard external `timeout` wrapper) before being written up:
//
// (1) ~ThreadPool() NEVER RETURNS once the pool has processed at least one
//     task — 100% reproducible. Root cause is in taskDispatcher()
//     (ThreadPool.hpp): the outer loop checks `running` and waits on
//     `taskGate`, but immediately enters an INNER `while (true)` with no
//     break condition that never re-checks `running`. Once the dispatcher
//     is unblocked for its first task it is permanently trapped calling
//     `taskQueue.dequeue()` (itself spins forever on an empty queue), so it
//     can never observe running=false again, and dispatcherThread.join()
//     in the destructor blocks forever. Practical impact: ANY code that
//     creates a ThreadPool and lets it clean up normally (RAII, a return,
//     stack unwinding) after running even one task hangs forever right
//     there — not an edge case, the ordinary lifecycle of the type.
//
// (2) TASKS CAN BE SILENTLY LOST under load — confirmed intermittently
//     (1 of 3 isolated 30,000-task runs stalled permanently; the other 2
//     completed instantly; in full `ctest` runs of this file it has fired
//     in 2-3 of the 4 task-volume sub-tests below per run — real and fires
//     readily under sustained load, just not deterministically). Most
//     likely mechanism, from reading
//     Worker::thread_loop(): it resets `poolFlag` (which is what makes the
//     dispatcher consider this worker eligible for a new task) BEFORE it
//     calls `gate.reset()`. If the dispatcher reassigns that now-eligible
//     worker in the gap between those two lines, the new assignment's
//     wake-up (gate.signaler() sets the internal flag to 1) gets clobbered
//     by this worker's own delayed gate.reset() (sets it back to 0) — the
//     new task sits in Worker::task forever, and the worker parks waiting
//     for a wake that already happened and was erased. Each time this
//     fires it also permanently removes one worker from the pool's
//     capacity (poolFlag stays stuck at 1 forever), so a pool that hits
//     this enough times degrades and can eventually stop making progress
//     at all. This is offered as the likely explanation based on source
//     review, not confirmed by forcing the exact interleaving.
//     Worst case, observed: if all 4 workers are eventually lost this way,
//     the dispatcher deadlocks entirely (spins forever failing to find a
//     free worker for the task it already dequeued), the internal 1024-slot
//     task queue fills up, and assignTask() itself starts spinning forever
//     too — so a test can get stuck before ever reaching its own bounded
//     wait for completion. That's why exactly_once_execution below has, in
//     some runs, hit the ctest-level TIMEOUT with no in-test diagnostic at
//     all rather than a clean [FAIL] — it never got past assigning tasks.
//
// Given (1), every test here uses its OWN heap-allocated ThreadPool that is
// deliberately never destroyed — there is no normal-path destruction that
// doesn't hang. One dedicated test (destructor_returns_after_use) attempts
// destruction under a bounded watchdog specifically to turn bug (1) into a
// clean, diagnosed failure instead of hanging the suite; its helper thread
// (and the ThreadPool it's stuck inside) is deliberately leaked if the
// destructor doesn't return in time, same trade used for the SIZE=1 ring
// buffer and AtomicFutex findings elsewhere in this suite.
//
// Given (2), each test also gets its OWN pool rather than sharing one, so
// one test tripping the race can't silently degrade capacity for tests
// that run after it and produce confusing, cascaded-looking failures.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Threading/ThreadPool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
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

template <typename Pred>
static bool waitFor(Pred pred, int timeoutSec) {
    int64_t t0 = nowNs();
    while (!pred()) {
        if (nowNs() - t0 > int64_t(timeoutSec) * 1'000'000'000) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

using AstraLib::Threading::ThreadPool;

// Every test heap-allocates its own pool via this and never frees it — see
// file header re: bug (1) (destructor hangs) and bug (2) (cross-test
// contamination from a degraded pool).
static ThreadPool* freshPool() { return new ThreadPool(); }

// ─────────────────────────────────────────────────────────────────────────────
// 1. Exactly-once execution under load: 20,000 tasks, each flips its own bit
//    in a bitmap. Catches lost tasks (bit never set, i.e. bug (2) above) and
//    double-execution (bit set twice).
// ─────────────────────────────────────────────────────────────────────────────
static void test_exactly_once_execution() {
    ThreadPool* pool = freshPool();
    constexpr int N = 20000;
    std::vector<std::atomic<int>> seen(N);
    for (auto& f : seen) f.store(0, std::memory_order_relaxed);
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        pool->assignTask([&, i] {
            int prev = seen[i].fetch_add(1, std::memory_order_acq_rel);
            CHECK_CTX(prev == 0, "task " << i << " executed more than once");
            completed.fetch_add(1, std::memory_order_release);
        });
    }

    bool done = waitFor([&] { return completed.load(std::memory_order_acquire) == N; }, 60);
    CHECK_CTX(done, "only " << completed.load() << "/" << N << " tasks completed within 60s "
              "— see bug (2) in file header (tasks can be silently lost under load)");

    for (int i = 0; i < N; ++i)
        CHECK_CTX(seen[i].load() == 1, "task " << i << " never executed");
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Tasks run off the calling thread, and across MORE THAN ONE worker (not
//    silently serialized onto a single thread every time).
// ─────────────────────────────────────────────────────────────────────────────
static void test_tasks_run_on_multiple_worker_threads() {
    ThreadPool* pool = freshPool();
    constexpr int N = 2000;
    std::thread::id mainId = std::this_thread::get_id();
    std::mutex mtx;
    std::set<std::thread::id> workerIds;
    std::atomic<int> completed{0};
    std::atomic<long> ranOnMainThread{0};

    for (int i = 0; i < N; ++i) {
        pool->assignTask([&] {
            std::thread::id id = std::this_thread::get_id();
            if (id == mainId) ranOnMainThread.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mtx);
                workerIds.insert(id);
            }
            completed.fetch_add(1, std::memory_order_release);
        });
    }

    bool done = waitFor([&] { return completed.load(std::memory_order_acquire) == N; }, 30);
    CHECK_CTX(done, "only " << completed.load() << "/" << N << " tasks completed within 30s");
    CHECK_CTX(ranOnMainThread.load() == 0, "tasks executed on the calling thread instead of a worker");

    std::lock_guard<std::mutex> lk(mtx);
    CHECK_CTX(workerIds.size() >= 2,
              "tasks were serialized onto only " << workerIds.size()
              << " worker thread(s) — expected work spread across multiple of the pool's 4 workers");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Repeated bursts: the pool must stay usable across multiple separate
//    assign-then-drain cycles, not just a single one-shot batch. (This is
//    also good exposure for bug (2) — more assign/drain transitions means
//    more chances to hit the reassignment race.)
// ─────────────────────────────────────────────────────────────────────────────
static void test_survives_repeated_bursts() {
    ThreadPool* pool = freshPool();
    constexpr int BURSTS = 20;
    constexpr int PER_BURST = 500;

    for (int b = 0; b < BURSTS; ++b) {
        std::atomic<int> completed{0};
        for (int i = 0; i < PER_BURST; ++i)
            pool->assignTask([&] { completed.fetch_add(1, std::memory_order_release); });

        bool done = waitFor([&] { return completed.load(std::memory_order_acquire) == PER_BURST; }, 15);
        CHECK_CTX(done, "burst " << b << ": only " << completed.load() << "/" << PER_BURST
                  << " tasks completed — pool did not remain usable across bursts");
        if (!done) return;   // pool is likely permanently degraded (bug 2); no point continuing
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Concurrent producers: multiple threads call assignTask() on the SAME
//    pool at once.
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_producers() {
    ThreadPool* pool = freshPool();
    constexpr int PRODUCERS = 8;
    constexpr int PER_PRODUCER = 2000;
    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;
    std::atomic<int> completed{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
        producers.emplace_back([&] {
            for (int i = 0; i < PER_PRODUCER; ++i)
                pool->assignTask([&] { completed.fetch_add(1, std::memory_order_release); });
        });
    for (auto& t : producers) t.join();

    bool done = waitFor([&] { return completed.load(std::memory_order_acquire) == TOTAL; }, 60);
    CHECK_CTX(done, "only " << completed.load() << "/" << TOTAL
              << " tasks completed within 60s under concurrent producers");
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Documents bug (1) from the file header. Destruction is attempted on a
//    helper thread under a bounded deadline; if it doesn't complete, that
//    thread — and the ThreadPool it's stuck inside — is deliberately
//    leaked rather than risking UB by abandoning it mid-teardown some
//    other way.
// ─────────────────────────────────────────────────────────────────────────────
static void test_destructor_returns_after_use() {
    ThreadPool* pool = freshPool();
    std::atomic<bool> ran{false};
    pool->assignTask([&] { ran.store(true, std::memory_order_release); });
    bool taskRan = waitFor([&] { return ran.load(std::memory_order_acquire); }, 10);
    CHECK_CTX(taskRan, "setup task never ran; can't test teardown behavior");
    if (!taskRan) return;

    std::atomic<bool> destroyed{false};
    std::thread destroyer([pool, &destroyed] {
        delete pool;   // expected to hang forever — see bug (1) in file header
        destroyed.store(true, std::memory_order_release);
    });

    bool ok = waitFor([&] { return destroyed.load(std::memory_order_acquire); }, 5);
    CHECK_CTX(ok,
        "BUG: ~ThreadPool() did not return within 5s of being destroyed after running "
        "one task — see bug (1) in file header for the mechanism (taskDispatcher's inner "
        "while(true) loop never re-checks `running`, so dispatcherThread.join() blocks "
        "forever). Practical impact: a ThreadPool cannot be cleanly destroyed after "
        "ordinary use — RAII cleanup, a function return, or stack unwinding will all hang.");
    if (ok) destroyer.join();
    else    destroyer.detach();   // leaked: the destructor call is permanently stuck
}

int main() {
    std::cout << "ThreadPool test suite  (hw threads: "
              << std::thread::hardware_concurrency() << ")\n" << std::endl;
    std::cout << "NOTE: no pool created here is ever cleanly destroyed — see file header. "
                 "Process exit (not a destructor) is what reclaims them all." << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"exactly_once_execution",               test_exactly_once_execution},
        {"tasks_run_on_multiple_worker_threads",  test_tasks_run_on_multiple_worker_threads},
        {"survives_repeated_bursts",              test_survives_repeated_bursts},
        {"concurrent_producers",                  test_concurrent_producers},
        {"destructor_returns_after_use",          test_destructor_returns_after_use},
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
    if (f == 0) { std::cout << "\nALL THREADPOOL TESTS PASSED" << std::endl; std::fflush(nullptr); std::_Exit(0); }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    std::fflush(nullptr);
    std::_Exit(1);   // never reach normal return / global destructors: no pool must be destroyed
}
