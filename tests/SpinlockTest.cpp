// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Atomic::Spinlock / SpinlockGuard.
//
// A lock's one job is mutual exclusion. The classic, maximally sensitive way
// to test that is a shared NON-atomic counter guarded by the lock: if the
// lock ever lets two threads into the critical section at once, the
// increment races and the final count comes up short. No lock bug hides
// from this test — a hundred threads hammering a plain `long` for a million
// total increments either lands exactly on the expected number, or it didn't
// provide exclusion.
//
// Because Spinlock has no blocking syscall, a broken lock shows up as a
// livelock (100% CPU, no progress) rather than a clean hang — so this suite
// uses the same watchdog pattern as the ring buffer stress test to turn that
// into a diagnosed failure instead of a frozen CI job.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Atomic/spinlock.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
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

// ── watchdog: a livelocked spinlock burns CPU forever without ever hanging
//    on a syscall, so a plain timeout command won't diagnose it cleanly ──────
static std::atomic<const char*> g_testName{"<none>"};
static std::atomic<int64_t> g_deadlineNs{INT64_MAX};

static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void watchdogLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int64_t dl = g_deadlineNs.load(std::memory_order_acquire);
        if (dl != INT64_MAX && nowNs() > dl) {
            std::fprintf(stderr, "\n[WATCHDOG] test '%s' exceeded its deadline "
                         "— the lock is livelocked (mutual exclusion broken, "
                         "or a thread deadlocked on itself).\n", g_testName.load());
            std::fflush(nullptr);
            std::_Exit(3);
        }
    }
}

static void run_test(const char* name, int timeoutSec, void (*fn)()) {
    std::cout << "[ RUN  ] " << name << std::endl;
    g_testName.store(name);
    long before = g_failures.load();
    int64_t t0 = nowNs();
    g_deadlineNs.store(t0 + int64_t(timeoutSec) * 1'000'000'000);
    fn();
    g_deadlineNs.store(INT64_MAX);
    double ms = double(nowNs() - t0) / 1e6;
    std::cout << (g_failures.load() == before ? "[  OK  ] " : "[ BAD  ] ")
              << name << "  (" << ms << " ms)" << std::endl;
}

using AstraLib::Atomic::Spinlock;
using AstraLib::Atomic::SpinlockGuard;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Mutual exclusion under heavy contention, raw lock()/unlock(). 16 threads
//    x 100,000 increments each on a plain (non-atomic) counter. Any missed
//    exclusion drops the final count below the arithmetic total.
// ─────────────────────────────────────────────────────────────────────────────
static void test_mutual_exclusion_raw() {
    constexpr int THREADS = 16;
    constexpr long PER = 100000;
    Spinlock lock;
    long counter = 0;

    std::vector<std::thread> ts;
    for (int i = 0; i < THREADS; ++i)
        ts.emplace_back([&] {
            for (long j = 0; j < PER; ++j) {
                lock.lock();
                counter = counter + 1;   // deliberately non-atomic read-modify-write
                lock.unlock();
            }
        });
    for (auto& t : ts) t.join();

    CHECK_CTX(counter == THREADS * PER,
              "expected " << (THREADS * PER) << " got " << counter
              << " — lock let concurrent threads into the critical section");
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Same property, through SpinlockGuard (RAII) instead of raw lock/unlock —
//    covers the path users are expected to actually use, including release
//    on an early `continue`/return out of the guarded scope.
// ─────────────────────────────────────────────────────────────────────────────
static void test_mutual_exclusion_raii_guard() {
    constexpr int THREADS = 16;
    constexpr long PER = 100000;
    Spinlock lock;
    long counter = 0;

    std::vector<std::thread> ts;
    for (int i = 0; i < THREADS; ++i)
        ts.emplace_back([&] {
            for (long j = 0; j < PER; ++j) {
                SpinlockGuard guard(lock);
                if (j % 7 == 0) {           // early-return path out of the guarded scope
                    counter = counter + 1;
                    continue;                // guard must still unlock via ~SpinlockGuard
                }
                counter = counter + 1;
            }
        });
    for (auto& t : ts) t.join();

    CHECK_CTX(counter == THREADS * PER,
              "expected " << (THREADS * PER) << " got " << counter
              << " — RAII guard failed to provide exclusion (or failed to "
              "release on the early-continue path)");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Custom waitTime constructor (backoff spin count) must not change
//    correctness — only the busy-wait tuning.
// ─────────────────────────────────────────────────────────────────────────────
static void test_mutual_exclusion_custom_waittime() {
    constexpr int THREADS = 16;
    constexpr long PER = 50000;
    Spinlock lock(50);   // much shorter pause spin than the default 1000
    long counter = 0;

    std::vector<std::thread> ts;
    for (int i = 0; i < THREADS; ++i)
        ts.emplace_back([&] {
            for (long j = 0; j < PER; ++j) {
                lock.lock();
                counter = counter + 1;
                lock.unlock();
            }
        });
    for (auto& t : ts) t.join();

    CHECK_CTX(counter == THREADS * PER,
              "expected " << (THREADS * PER) << " got " << counter
              << " with a custom waitTime — correctness must not depend on backoff tuning");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Critical section that does real (slower) work, not just an increment —
//    proves exclusion holds even when a thread spends a while INSIDE the
//    lock (a window where a broken lock is more likely to let a second
//    thread slip in), using a multi-step read-modify-write that's very
//    sensitive to interleaving (swap two halves of a struct).
//
//    THREADS/PER scale down on machines with few cores. This is the one test
//    in this file that yields WHILE HOLDING the lock — combined with more
//    threads than cores, that creates a scheduler convoy (every other thread
//    is a runnable spinner, so the OS has no signal the actual holder is the
//    one that needs to run next). Confirmed directly: the original 8
//    threads/20000 iters, constrained to 2 cores (matching a GitHub Actions
//    runner) via `taskset -c 0,1`, was STILL RUNNING after 3 minutes at 100%
//    CPU on both cores — not a hang, just catastrophically slow, and
//    reproduced identically on GitHub's own 2-core runner. Tests 1-3 in this
//    file use MORE threads (16) than this one but don't yield while holding,
//    and stayed fast (~15ms) even on 2 cores — the yield-while-held pattern
//    is what makes oversubscription dangerous here, not thread count alone.
// ─────────────────────────────────────────────────────────────────────────────
struct Torn { long a; long b; };   // invariant: a must always equal -b

static void test_mutual_exclusion_wide_critical_section() {
    unsigned hw = std::thread::hardware_concurrency();
    const bool constrained = hw != 0 && hw < 8;
    const int THREADS = constrained ? std::max<int>(2, int(hw)) : 8;
    const int PER = constrained ? 5000 : 20000;
    Spinlock lock;
    Torn shared{0, 0};
    std::atomic<long> violations{0};

    std::vector<std::thread> ts;
    for (int i = 0; i < THREADS; ++i)
        ts.emplace_back([&] {
            for (int j = 0; j < PER; ++j) {
                lock.lock();
                long a = shared.a;
                std::this_thread::yield();   // widen the window a broken lock could exploit
                shared.a = a + 1;
                shared.b = -(a + 1);
                if (shared.a != -shared.b) violations.fetch_add(1, std::memory_order_relaxed);
                lock.unlock();
            }
        });
    for (auto& t : ts) t.join();

    CHECK_CTX(violations.load() == 0,
              violations.load() << " invariant violation(s) — another thread "
              "observed/mutated shared state while this thread held the lock");
    CHECK_CTX(shared.a == THREADS * PER,
              "expected shared.a == " << (THREADS * PER) << " got " << shared.a);
}

int main() {
    std::cout << "Spinlock test suite  (hw threads: "
              << std::thread::hardware_concurrency() << ")\n" << std::endl;

    std::thread(watchdogLoop).detach();

    run_test("mutual_exclusion_raw",             30, test_mutual_exclusion_raw);
    run_test("mutual_exclusion_raii_guard",       30, test_mutual_exclusion_raii_guard);
    run_test("mutual_exclusion_custom_waittime",  30, test_mutual_exclusion_custom_waittime);
    run_test("mutual_exclusion_wide_critical_section", 30, test_mutual_exclusion_wide_critical_section);

    long f = g_failures.load();
    if (f == 0) { std::cout << "\nALL SPINLOCK TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
