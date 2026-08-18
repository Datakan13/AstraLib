// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Threading::ThreadPool.
//
// (1) ~ThreadPool() HANGING — FIXED. Used to never return once the pool had
//     processed at least one task, 100% reproducible: taskDispatcher()'s
//     inner loop had no break condition and never re-checked `running`, so
//     dispatcherThread.join() in the destructor blocked forever. Fixed by
//     bounding the inner loop with `running && !taskQueue.isEmpty()`
//     (ThreadPool.hpp) plus a new AtomicRingBuffer::isEmpty(). Verified: 9
//     consecutive runs of destructor_returns_after_use (1 full-suite + 8
//     standalone) all completed cleanly. destructor_returns_after_use below
//     is kept as the regression guard for this — a return to hanging would
//     show up there immediately.
//
// (2) TASKS CAN BE SILENTLY LOST under load — FIXED. Worker::thread_loop()
//     used to reset `poolFlag` (which is what makes the dispatcher consider
//     a worker eligible for a new task) BEFORE calling `gate.reset()`. If
//     the dispatcher reassigned that now-eligible worker in the gap between
//     those two lines, the new assignment's wake-up (gate.signaler() sets
//     the internal flag to 1) got clobbered by this worker's own delayed
//     gate.reset() (sets it back to 0) — the new task sat in Worker::task
//     forever, and the worker parked waiting for a wake that already
//     happened and was erased. Each hit also permanently removed one worker
//     from the pool's capacity (poolFlag stuck at 1 forever), so a pool that
//     hit this enough times degraded and could eventually stop making
//     progress entirely. Fixed by swapping the order — gate.reset() now
//     runs BEFORE poolFlag.store(0, ...), so a worker is never marked
//     reassignable until its own gate has already been cleared, closing the
//     window rather than narrowing it. Checked for a residual lost-wakeup
//     risk in the new ordering and found none: ThreadGate::waiter() always
//     re-checks its flag fresh against a literal 0 immediately before each
//     FUTEX_WAIT, so a signaler() landing anywhere is still never missed.
//     Verified empirically too: 35 total clean runs across two independent
//     passes (10 + 25), each exercising 300 burst cycles and 16,000
//     concurrently-produced tasks — survives_repeated_bursts below was
//     deliberately pushed from 20 to 300 bursts specifically to give this
//     bug room to surface, and kept at 300 as the regression guard now.
//
// (3) DISPATCHER BUSY-SPINS INSTEAD OF SLEEPING WHEN IDLE — FIXED. taskGate's
//     flag used to be set once by the first assignTask()'s signaler() and
//     never reset anywhere in ThreadPool itself, so taskGate.waiter() stopped
//     blocking after the very first task and the dispatcher spun on isEmpty()
//     checks forever — pinning a full CPU core for the pool's remaining
//     lifetime. Fixed by adding `if (taskQueue.isEmpty()) { taskGate.reset(); }`
//     after the dispatcher's inner loop. dispatcher_idle_behavior below
//     measures process CPU time over a deliberately idle window as the
//     regression guard: dropped from ~10s to 0.000037s over a 2s window
//     after the fix.
//
// Every test here still uses its OWN heap-allocated ThreadPool that is
// never explicitly destroyed, and each gets its own pool rather than
// sharing one. That posture predates fixes (1)-(3) and is now conservative
// rather than strictly required, but there's no cost to keeping it, so it
// stays — isolating each test's pool keeps one test's finding from ever
// being confused with another's.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Threading/ThreadPool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>
#include <sys/resource.h>
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
//    assign-then-drain cycles, not just a single one-shot batch. This is
//    also the main hunting ground for bug (2) — each burst's assign/drain
//    transition is a fresh chance to hit the reassignment race, so BURSTS
//    is deliberately high (300, not a token few) to give it real room to
//    show up before this gets called fixed. If it hits, the pool is likely
//    permanently degraded, so we bail after the first failure rather than
//    generating 299 more redundant reports.
// ─────────────────────────────────────────────────────────────────────────────
static void test_survives_repeated_bursts() {
    ThreadPool* pool = freshPool();
    constexpr int BURSTS = 300;
    constexpr int PER_BURST = 500;

    for (int b = 0; b < BURSTS; ++b) {
        std::atomic<int> completed{0};
        for (int i = 0; i < PER_BURST; ++i)
            pool->assignTask([&] { completed.fetch_add(1, std::memory_order_release); });

        bool done = waitFor([&] { return completed.load(std::memory_order_acquire) == PER_BURST; }, 15);
        CHECK_CTX(done, "burst " << b << "/" << BURSTS << ": only " << completed.load() << "/" << PER_BURST
                  << " tasks completed — pool did not remain usable across bursts (see bug (2) in file header)");
        if (!done) return;   // pool is likely permanently degraded; no point continuing
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
// 5. Regression guard for bug (1) (now fixed — see file header). Destruction
//    is still attempted on a helper thread under a bounded deadline rather
//    than inline: if the fix ever regresses, this reports a clean diagnosis
//    instead of hanging the whole suite, and the stuck thread (plus the
//    ThreadPool it's stuck inside) is deliberately leaked rather than
//    risking UB by abandoning it mid-teardown some other way.
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
        delete pool;
        destroyed.store(true, std::memory_order_release);
    });

    bool ok = waitFor([&] { return destroyed.load(std::memory_order_acquire); }, 5);
    CHECK_CTX(ok,
        "REGRESSION: ~ThreadPool() did not return within 5s of being destroyed after "
        "running one task. This was bug (1) — see file header for the original mechanism "
        "and the fix (taskDispatcher's inner loop now bounded by `running && "
        "!taskQueue.isEmpty()`). If this fires, that fix broke or was reverted.");
    if (ok) destroyer.join();
    else    destroyer.detach();   // leaked: the destructor call is permanently stuck
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Does the dispatcher actually sleep when idle, or busy-spin forever once
//    it has processed its first task? taskGate's underlying flag is set by
//    signaler() and never explicitly reset anywhere in ThreadPool itself
//    (only each Worker's OWN separate gate gets reset) — so the suspicion is
//    that taskGate.waiter() stops blocking after the very first task, and
//    the dispatcher just spins on isEmpty() checks forever afterward. That's
//    not a correctness bug (nothing is lost or wrong), but it means the pool
//    permanently pins a full CPU core the moment it's used, which matters
//    for anyone running this in production. Measured via total PROCESS CPU
//    time (getrusage(RUSAGE_SELF)) over an otherwise-idle window — this is
//    whole-process, not just this test's own pool, and since every prior
//    test in this file also leaks its pool (deliberately — see file
//    header), their dispatchers are still alive and still spinning too by
//    the time this test runs. That's not a flaw in the measurement; if
//    anything it's a more honest demonstration of the real problem: every
//    ThreadPool ever used and abandoned keeps burning a full core forever,
//    and the cost compounds with each one. Near-zero would mean genuine
//    sleeping; anything approaching or exceeding the wall-clock duration
//    means at least one thread (this run observed several, cumulatively)
//    is spinning at ~100%.
// ─────────────────────────────────────────────────────────────────────────────
static double cpuTimeSeconds() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) / 1e6)
         + (double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) / 1e6);
}

static void test_dispatcher_idle_behavior() {
    ThreadPool* pool = freshPool();
    std::atomic<bool> ran{false};
    pool->assignTask([&] { ran.store(true, std::memory_order_release); });   // warm it up
    bool taskRan = waitFor([&] { return ran.load(std::memory_order_acquire); }, 10);
    CHECK_CTX(taskRan, "setup task never ran; can't measure idle behavior");
    if (!taskRan) return;

    constexpr double IDLE_SECONDS = 2.0;
    double before = cpuTimeSeconds();
    std::this_thread::sleep_for(std::chrono::duration<double>(IDLE_SECONDS));
    double cpuUsed = cpuTimeSeconds() - before;

    std::cout << "  [idle window: " << IDLE_SECONDS << "s wall time, " << cpuUsed
              << "s of process CPU time consumed]" << std::endl;
    CHECK_CTX(cpuUsed < IDLE_SECONDS * 0.5,
        "ThreadPool dispatchers appear to busy-spin instead of sleeping when idle: this "
        "process consumed " << cpuUsed << "s of CPU time over a " << IDLE_SECONDS << "s "
        "wall-clock idle window (expected near-zero if they properly block on the futex "
        "gate; this number is cumulative across every pool this test binary has created and "
        "left running so far, not just this test's own pool — see the comment above). Likely "
        "cause: taskGate's flag is set once by the first assignTask()'s signaler() and never "
        "reset anywhere in ThreadPool, so taskGate.waiter() stops blocking after the first "
        "task and each dispatcher just spins checking isEmpty() forever — burning a full CPU "
        "core for the remaining lifetime of every pool ever used.");
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
        {"dispatcher_idle_behavior",              test_dispatcher_idle_behavior},
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
