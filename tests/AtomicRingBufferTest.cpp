// ─────────────────────────────────────────────────────────────────────────────
// Basic functional coverage for AstraLib::Buffers::AtomicRingBuffer — the
// parts NOT covered by AtomicRingBufferStressTest.cpp's adversarial MPMC
// suite: isEmpty(), the enqueue(const A&) copy overload, emplaceEnqueue's
// trivially-destructible branch, non-default BATCH_SIZE, and batchDequeue's
// count-vs-BATCH_SIZE clamping (regression test for a real, single-threaded
// buffer overflow — see test 6's own comment).
//
// isEmpty() in particular just became safety-critical: ThreadPool's
// taskDispatcher now uses it to decide when to stop looking for work, so a
// wrong answer here has real consequences upstream, not just a cosmetic one.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Buffers/atomicRingBuffer.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

namespace RB = AstraLib::Buffers;

static long g_failures = 0;

#define CHECK_CTX(cond, ctx)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__            \
                      << "  CHECK(" #cond ")  " << ctx << "\n";              \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_CTX(cond, "")

// ─────────────────────────────────────────────────────────────────────────────
// 1. isEmpty(): starts true, false after enqueue, true again after the
//    matching dequeue.
// ─────────────────────────────────────────────────────────────────────────────
static void test_isempty_basic_transitions() {
    RB::AtomicRingBuffer<int, 4> q;
    CHECK_CTX(q.isEmpty(), "fresh buffer should be empty");

    q.enqueue(1);
    CHECK_CTX(!q.isEmpty(), "buffer with one item should not be empty");

    q.enqueue(2);
    CHECK_CTX(!q.isEmpty(), "buffer with two items should not be empty");

    (void)q.dequeue();
    CHECK_CTX(!q.isEmpty(), "one item still pending — should not be empty");

    (void)q.dequeue();
    CHECK_CTX(q.isEmpty(), "buffer drained to zero — should be empty again");
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. isEmpty() must not report "empty" while a producer has claimed a ticket
//    but hasn't published yet — otherwise a consumer (e.g. ThreadPool's
//    dispatcher) could wrongly conclude there's no work and stop looking.
//    A slow-to-assign type widens that claimed-but-unpublished window so the
//    check lands inside it reliably instead of by luck.
// ─────────────────────────────────────────────────────────────────────────────
struct SlowPublish {
    int v = 0;
    SlowPublish() = default;
    explicit SlowPublish(int x) : v(x) {}
    SlowPublish(SlowPublish&&) noexcept = default;
    SlowPublish& operator=(SlowPublish&& o) noexcept {
        v = o.v;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // widen the window
        return *this;
    }
};

static void test_isempty_stays_false_during_inflight_publish() {
    RB::AtomicRingBuffer<SlowPublish, 4> q;
    CHECK(q.isEmpty());

    std::atomic<bool> started{false};
    std::thread producer([&] {
        started.store(true, std::memory_order_release);
        q.enqueue(SlowPublish(42));   // ticket claimed immediately; publish takes ~200ms
    });

    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // land inside the slow publish

    CHECK_CTX(!q.isEmpty(),
              "isEmpty() reported true while a producer had already claimed a ticket "
              "but not yet published — a consumer could wrongly give up looking for work");

    producer.join();
    CHECK_CTX(!q.isEmpty(), "one published item pending — should not be empty");

    SlowPublish out = q.dequeue();
    CHECK_CTX(out.v == 42, "expected 42, got " << out.v);
    CHECK_CTX(q.isEmpty(), "drained — should be empty again");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. enqueue(const A&): must copy, not move from, the caller's lvalue. Uses a
//    heap-backed std::string (long enough to defeat SSO) so an accidental
//    move would be observable as the source going empty/moved-from.
// ─────────────────────────────────────────────────────────────────────────────
static void test_enqueue_const_ref_copies_not_moves() {
    RB::AtomicRingBuffer<std::string, 4> q;
    std::string original = "hello-world-content-long-enough-to-defeat-sso";
    std::string originalCopy = original;

    q.enqueue(original);   // lvalue -> must bind to enqueue(const A&)

    CHECK_CTX(original == originalCopy,
              "enqueue(const A&) modified/moved-from the caller's lvalue: got \"" << original << "\"");

    std::string out = q.dequeue();
    CHECK_CTX(out == originalCopy, "dequeued value doesn't match what was enqueued: \"" << out << "\"");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. emplaceEnqueue's trivially-destructible branch. The stress suite's
//    lifetime canary type is deliberately NON-trivial (to catch double-
//    destruction), so it only ever exercises the destroy-then-placement-new
//    else-branch — this is the only test that hits the plain placement-new
//    path for a trivially destructible type.
// ─────────────────────────────────────────────────────────────────────────────
struct TrivialPayload { int a; int b; };
static_assert(std::is_trivially_destructible_v<TrivialPayload>,
              "this test is specifically for the trivially-destructible branch");

static void test_emplace_enqueue_trivially_destructible_branch() {
    RB::AtomicRingBuffer<TrivialPayload, 4> q;
    for (int i = 0; i < 4; ++i)
        q.emplaceEnqueue(TrivialPayload{i, i * 10});
    for (int i = 0; i < 4; ++i) {
        TrivialPayload p = q.dequeue();
        CHECK_CTX(p.a == i && p.b == i * 10,
                  "expected {" << i << "," << (i * 10) << "} got {" << p.a << "," << p.b << "}");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Non-default BATCH_SIZE: everything elsewhere in the suite uses the
//    default (50), which never proves the template parameter actually
//    changes the underlying array size correctly.
// ─────────────────────────────────────────────────────────────────────────────
static void test_custom_batch_size_exact_request() {
    constexpr int CUSTOM_BATCH = 10;
    RB::AtomicRingBuffer<int, 1024, CUSTOM_BATCH> q;
    for (int i = 0; i < CUSTOM_BATCH; ++i) q.enqueue(int(i));

    std::array<int, CUSTOM_BATCH>* batch = q.batchDequeue(CUSTOM_BATCH);
    for (int i = 0; i < CUSTOM_BATCH; ++i)
        CHECK_CTX((*batch)[i] == i, "custom BATCH_SIZE=" << CUSTOM_BATCH << " index " << i
                  << " got " << (*batch)[i]);
}

static void test_custom_batch_size_partial_request() {
    constexpr int CUSTOM_BATCH = 20;
    RB::AtomicRingBuffer<int, 1024, CUSTOM_BATCH> q;
    for (int i = 0; i < 7; ++i) q.enqueue(int(i + 100));

    std::array<int, CUSTOM_BATCH>* batch = q.batchDequeue(7);   // fewer than BATCH_SIZE
    for (int i = 0; i < 7; ++i)
        CHECK_CTX((*batch)[i] == i + 100, "partial request into BATCH_SIZE=" << CUSTOM_BATCH
                  << " index " << i << " got " << (*batch)[i]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Regression test: batchDequeue(count) must clamp to BATCH_SIZE, not
//    overflow it. batchDequeueArray (std::array<A, BATCH_SIZE>) sits
//    immediately before buffer[SIZE] in the class layout, so an unclamped
//    request used to write straight past it into the ring buffer's own
//    data — confirmed via UBSan before this was fixed:
//    "index 3 out of bounds for type 'int [2]'", no concurrency required.
//    A plain build can't see the sanitizer signal, so this checks the
//    LOGICAL contract instead: the call must return exactly BATCH_SIZE
//    items, and the remainder must still be sitting in the queue — in
//    order, not dropped, not corrupted. Run this suite under
//    `-DENABLE_ASAN=ON` (pulls in UBSan too) to get the hard bounds-check
//    signal directly on top of this.
// ─────────────────────────────────────────────────────────────────────────────
static void test_batch_dequeue_clamps_to_batch_size() {
    constexpr int BATCH = 2;
    RB::AtomicRingBuffer<int, 8, BATCH> q;
    for (int i = 0; i < 5; ++i) q.enqueue(int(100 + i));

    std::array<int, BATCH>* batch = q.batchDequeue(5);   // request > BATCH_SIZE
    CHECK_CTX((*batch)[0] == 100, "batch[0] expected 100 got " << (*batch)[0]);
    CHECK_CTX((*batch)[1] == 101, "batch[1] expected 101 got " << (*batch)[1]);

    // The 3 items past the clamp must still be in the queue, in FIFO order —
    // proves the overflow didn't silently drop or corrupt them.
    int r0 = q.dequeue(), r1 = q.dequeue(), r2 = q.dequeue();
    CHECK_CTX(r0 == 102, "remaining[0] expected 102 got " << r0);
    CHECK_CTX(r1 == 103, "remaining[1] expected 103 got " << r1);
    CHECK_CTX(r2 == 104, "remaining[2] expected 104 got " << r2);
}

int main() {
    std::cout << "AtomicRingBuffer basic functional test suite\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"isempty_basic_transitions",                     test_isempty_basic_transitions},
        {"isempty_stays_false_during_inflight_publish",    test_isempty_stays_false_during_inflight_publish},
        {"enqueue_const_ref_copies_not_moves",             test_enqueue_const_ref_copies_not_moves},
        {"emplace_enqueue_trivially_destructible_branch",  test_emplace_enqueue_trivially_destructible_branch},
        {"custom_batch_size_exact_request",                test_custom_batch_size_exact_request},
        {"custom_batch_size_partial_request",              test_custom_batch_size_partial_request},
        {"batch_dequeue_clamps_to_batch_size",              test_batch_dequeue_clamps_to_batch_size},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        long before = g_failures;
        t.fn();
        std::cout << (g_failures == before ? "[  OK  ] " : "[ BAD  ] ") << t.name << std::endl;
    }

    if (g_failures == 0) { std::cout << "\nALL ATOMICRINGBUFFER TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
