// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Atomic::AtomicFutex.
//
// wait()'s own doc comment states the contract: "Will wait unless there has
// been a wake call." That's directly testable: call wake() fully before
// wait(), and wait() must return promptly rather than block.
//
// It doesn't. This suite proves it (test_wake_before_wait_should_not_block),
// with a bounded wait rather than an indefinite one — a stuck waiter thread
// is detached (never joined) and its AtomicFutex is heap-allocated and
// deliberately leaked rather than destroyed, because a thread that may still
// be parked in the FUTEX_WAIT syscall referencing that memory makes
// destroying it undefined behavior. That trade (a deliberate small leak) is
// what lets this suite report a clean diagnosis instead of hanging CI or
// risking a later crash on freed memory.
//
// Root cause: wait() snapshots `expected = futex_val.load()` at call time.
// If wake() already ran, that snapshot already reflects the post-wake value,
// so the kernel's FUTEX_WAIT comparison (*uaddr == expected) succeeds and it
// sleeps — with no further wake coming. Turn-taking usage (always wait()
// before the matching wake()) never hits this, which is presumably why it
// hasn't surfaced before; see test_repeated_turntaking_cycles for that path
// working correctly.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Atomic/atomicFutex.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

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

using Futex = AstraLib::Atomic::AtomicFutex;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Normal turn-taking: waiter calls wait() first, then gets woken. This is
//    the intended usage pattern (nothing else in the library uses AtomicFutex
//    directly anymore — AtomicRingBuffer's futex-based dequeue methods were
//    removed), and it must work.
// ─────────────────────────────────────────────────────────────────────────────
static void test_wait_then_wake_normal_order() {
    Futex fx;
    std::atomic<bool> waiting{false}, returned{false};

    std::thread waiter([&] {
        waiting.store(true, std::memory_order_release);
        fx.wait();
        returned.store(true, std::memory_order_release);
    });

    while (!waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // let it reach the syscall
    fx.wake();

    int64_t t0 = nowNs();
    while (!returned.load(std::memory_order_acquire) && nowNs() - t0 < 2'000'000'000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK_CTX(returned.load(), "wait() never returned within 2s of a wake() "
              "issued after it started waiting");
    if (returned.load()) waiter.join(); else waiter.detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Documented contract: wake() BEFORE wait() must not block wait().
//    Empirically confirmed broken — see file header. Heap-allocated futex,
//    deliberately leaked if the waiter is still parked when we give up,
//    since destroying it under a live syscall reference would be UB.
// ─────────────────────────────────────────────────────────────────────────────
static void test_wake_before_wait_should_not_block() {
    auto* fx = new Futex();
    fx->wake();   // fully completed before wait() is ever called

    auto* returned = new std::atomic<bool>{false};
    std::thread waiter([fx, returned] {
        fx->wait();
        returned->store(true, std::memory_order_release);
    });

    int64_t t0 = nowNs();
    while (!returned->load(std::memory_order_acquire) && nowNs() - t0 < 2'000'000'000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    bool ok = returned->load();
    CHECK_CTX(ok,
        "BUG: wait() blocked even though wake() was called before it — violates "
        "wait()'s own doc comment (\"Will wait unless there has been a wake call\"). "
        "Root cause: wait()'s expected-value snapshot is taken AFTER wake() already "
        "ran, so the kernel sees futex_val already equal to `expected` and sleeps "
        "with no further wake pending. Fix needs wait() to detect \"already woken\" "
        "before snapshotting, e.g. compare against a value captured before the "
        "caller could have missed a wake, or have wake() store a distinct "
        "\"pending\" state wait() checks first.");
    if (ok) { waiter.join(); delete fx; delete returned; }
    else    { waiter.detach(); /* leaked: still possibly parked in the syscall */ }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Repeated turn-taking cycles on ONE futex (futex_val keeps incrementing
//    via fetch_add — never resets). Proper wait-then-wake sequencing must
//    keep working across many cycles, proving the monotonic counter design
//    is sound for its intended usage pattern despite issue #2 above.
// ─────────────────────────────────────────────────────────────────────────────
static void test_repeated_turntaking_cycles() {
    constexpr int CYCLES = 200;
    Futex fx;
    for (int c = 0; c < CYCLES; ++c) {
        std::atomic<bool> waiting{false}, returned{false};
        std::thread waiter([&] {
            waiting.store(true, std::memory_order_release);
            fx.wait();
            returned.store(true, std::memory_order_release);
        });
        while (!waiting.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        fx.wake();

        int64_t t0 = nowNs();
        while (!returned.load(std::memory_order_acquire) && nowNs() - t0 < 2'000'000'000)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        bool ok = returned.load();
        CHECK_CTX(ok, "cycle " << c << ": wait()/wake() turn-taking failed to hand off");
        if (ok) waiter.join();
        else { waiter.detach(); break; }   // don't keep hammering a futex a thread is stuck on
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. wake() count semantics: FUTEX_WAKE is issued with count=1, so it should
//    release at most ONE waiter, not broadcast to all of them. Two waiters
//    both parked (turn-taking: both call wait() before any wake()); one
//    wake() must release exactly one, and it takes two wake()s to free both.
// ─────────────────────────────────────────────────────────────────────────────
static void test_wake_releases_one_waiter_not_both() {
    Futex fx;
    std::atomic<int> waitingCount{0};
    std::atomic<bool> returned0{false}, returned1{false};

    std::thread w0([&] { waitingCount.fetch_add(1); fx.wait(); returned0.store(true, std::memory_order_release); });
    std::thread w1([&] { waitingCount.fetch_add(1); fx.wait(); returned1.store(true, std::memory_order_release); });

    while (waitingCount.load() < 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // let both reach the syscall

    fx.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int releasedAfterOne = int(returned0.load()) + int(returned1.load());
    CHECK_CTX(releasedAfterOne == 1,
              "expected exactly 1 waiter released by a single wake(), got " << releasedAfterOne);

    fx.wake();   // release the second one (cleanup, and confirms the primitive isn't a one-shot)
    int64_t t0 = nowNs();
    while (int(returned0.load()) + int(returned1.load()) < 2 && nowNs() - t0 < 2'000'000'000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    int releasedAfterTwo = int(returned0.load()) + int(returned1.load());
    CHECK_CTX(releasedAfterTwo == 2,
              "expected both waiters released after a second wake(), got " << releasedAfterTwo);

    if (returned0.load()) w0.join(); else w0.detach();
    if (returned1.load()) w1.join(); else w1.detach();
}

int main() {
    std::cout << "AtomicFutex test suite\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"wait_then_wake_normal_order",       test_wait_then_wake_normal_order},
        {"wake_before_wait_should_not_block", test_wake_before_wait_should_not_block},
        {"repeated_turntaking_cycles",        test_repeated_turntaking_cycles},
        {"wake_releases_one_waiter_not_both", test_wake_releases_one_waiter_not_both},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        long before = g_failures.load();
        t.fn();
        std::cout << (g_failures.load() == before ? "[  OK  ] " : "[ BAD  ] ") << t.name << std::endl;
    }

    long f = g_failures.load();
    if (f == 0) { std::cout << "\nALL ATOMICFUTEX TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
