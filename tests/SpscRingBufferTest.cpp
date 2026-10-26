// ─────────────────────────────────────────────────────────────────────────────
// Test suite for AstraLib::Buffers::spscRingBuffer — single producer, single
// consumer, one ready flag (ticket) per slot.
//
// The protocol is: the producer waits for its slot's ticket to read 0, writes
// the data, then release-stores 1; the consumer waits for 1 (acquire), moves
// the data out, then release-stores 0. Each side keeps a private cursor
// (lastWritten / lastRead) that only it touches. So the things that can go
// wrong are:
//
//   * the data write isn't ordered before the ticket store (or the consumer's
//     read before its reset) — shows up as torn or stale payloads, and as a
//     TSan race on `data`;
//   * the two cursors disagree about where the ring wraps — shows up as
//     out-of-order or lost items, or a hang at the boundary;
//   * full/empty blocking is wrong — producer overwrites an unread slot, or a
//     side never wakes;
//   * emplaceEnqueue's destroy + placement-new mishandles object lifetimes.
//
// Earlier drafts of this header had both of the first two bugs (an `==` where
// `=` was meant, so enqueue silently stored nothing, and relaxed ticket
// ordering). Test 1 and test 6 are the ones that catch them.
//
// The queue blocks by spinning, so a bug presents as a hang rather than an
// error. A watchdog thread turns that into a diagnosed failure, and ctest's
// TIMEOUT backs it up.
//
// Scale knob: ASTRA_STRESS_SCALE=N divides item counts by N (use under TSan).
// Only ever two threads, so this is safe on the 2-vCPU CI runner as-is.
// ─────────────────────────────────────────────────────────────────────────────
#include <AstraLib/Buffers/spscRingBuffer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace B = AstraLib::Buffers;

// ── layout expectations ─────────────────────────────────────────────────────
// 128, not AtomicRingBuffer's 64, on purpose: measured ~18% faster than 64
// for this queue (docs/tests.md, finding 11). Re-measure before changing.
static_assert(sizeof(B::spscSlot<int>) == 128,  "spscSlot<int> should be padded to 128B");
static_assert(alignof(B::spscSlot<int>) == 128, "spscSlot should be 128-byte aligned");

// ── thread-safe CHECK machinery ─────────────────────────────────────────────
static std::atomic<long> g_failures{0};
static std::mutex g_outMtx;

#define CHECK_CTX(cond, ctx)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            long n_ = g_failures.fetch_add(1, std::memory_order_relaxed) + 1;  \
            if (n_ <= 50) {                                                    \
                std::lock_guard<std::mutex> lk_(g_outMtx);                     \
                std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__          \
                          << "  CHECK(" #cond ")  " << ctx << "\n";            \
                if (n_ == 50) std::cerr << "[FAIL] (further failures suppressed)\n"; \
            }                                                                  \
        }                                                                      \
    } while (0)

#define CHECK(cond) CHECK_CTX(cond, "")

// ── watchdog ────────────────────────────────────────────────────────────────
static std::atomic<const char*> g_testName{"<none>"};
static std::atomic<const char*> g_testHint{""};
static std::atomic<int64_t>  g_deadlineNs{INT64_MAX};
static std::atomic<uint64_t> g_produced{0};
static std::atomic<uint64_t> g_consumed{0};

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
                "\n[WATCHDOG] test '%s' exceeded its deadline — the queue is "
                "deadlocked or livelocked.\n"
                "[WATCHDOG] progress: produced=%llu consumed=%llu\n"
                "[WATCHDOG] %s\n",
                g_testName.load(),
                (unsigned long long)g_produced.load(),
                (unsigned long long)g_consumed.load(),
                g_testHint.load());
            std::fflush(nullptr);
            std::_Exit(3);
        }
    }
}

static void run_test(const char* name, int timeoutSec, const char* hint,
                     void (*fn)()) {
    std::cout << "[ RUN  ] " << name << std::endl;
    g_produced.store(0); g_consumed.store(0);
    g_testName.store(name); g_testHint.store(hint);
    long before = g_failures.load();
    int64_t t0 = nowNs();
    g_deadlineNs.store(t0 + int64_t(timeoutSec) * 1'000'000'000);
    fn();
    g_deadlineNs.store(INT64_MAX);
    double ms = double(nowNs() - t0) / 1e6;
    std::cout << (g_failures.load() == before ? "[  OK  ] " : "[ BAD  ] ")
              << name << "  (" << ms << " ms)" << std::endl;
}

static uint64_t g_scale = 1;
static uint64_t scaled(uint64_t base, uint64_t minimum = 1000) {
    uint64_t v = base / g_scale;
    return v < minimum ? minimum : v;
}

// The ring is SIZE * 128+ bytes; keep it off the stack.
template <typename A, std::size_t N>
static std::unique_ptr<B::spscRingBuffer<A, N>> makeQueue() {
    return std::make_unique<B::spscRingBuffer<A, N>>();
}

// Streams 1..n through a queue on two threads and checks strict FIFO order.
template <std::size_t N>
static void streamInOrder(uint64_t n) {
    auto q = makeQueue<uint64_t, N>();
    std::thread consumer([&] {
        uint64_t expect = 1;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t v = q->dequeue();
            CHECK_CTX(v == expect, "SIZE=" << N << ": expected " << expect << " got " << v);
            expect = v + 1;   // resync so one fault doesn't cascade into n failures
            g_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (uint64_t i = 1; i <= n; ++i) {
        q->enqueue(uint64_t(i));
        g_produced.fetch_add(1, std::memory_order_relaxed);
    }
    consumer.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Single thread: values come back exactly, in order, across many full
//    wraparounds of a small ring. Fills to capacity each round, so every
//    slot — including the wrap from index SIZE-1 back to 0 — is exercised.
//    This is the test that catches an enqueue that never stores the value.
// ─────────────────────────────────────────────────────────────────────────────
static void test_single_thread_fifo_wraparound() {
    constexpr std::size_t N = 4;
    auto q = makeQueue<int, N>();
    int next = 1, expect = 1;
    for (int round = 0; round < 1000; ++round) {
        // Vary the fill level so the cursors drift relative to the wrap point.
        const int fill = 1 + round % int(N);
        for (int i = 0; i < fill; ++i) q->enqueue(int(next++));
        for (int i = 0; i < fill; ++i) {
            int v = q->dequeue();
            CHECK_CTX(v == expect, "round " << round << ": expected " << expect << " got " << v);
            expect = v + 1;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Capacity is exactly BUFFER_SIZE: SIZE enqueues on an empty ring must all
//    complete without a consumer; the next one must block until a dequeue
//    frees a slot — and must not overwrite the unread value in that slot.
// ─────────────────────────────────────────────────────────────────────────────
static void test_full_ring_blocks_producer() {
    constexpr std::size_t N = 4;
    auto q = makeQueue<int, N>();
    for (int i = 1; i <= int(N); ++i) q->enqueue(int(i));   // must not block

    std::atomic<bool> done{false};
    std::thread producer([&] {
        q->enqueue(int(N + 1));
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_CTX(!done.load(std::memory_order_acquire),
              "enqueue into a full ring returned without a free slot");

    int first = q->dequeue();
    CHECK_CTX(first == 1, "overwrote an unread slot: first dequeue gave " << first);
    producer.join();
    CHECK(done.load());
    for (int i = 2; i <= int(N + 1); ++i) {
        int v = q->dequeue();
        CHECK_CTX(v == i, "after unblocking: expected " << i << " got " << v);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Empty ring: dequeue must block until a value is published, then return
//    exactly that value.
// ─────────────────────────────────────────────────────────────────────────────
static void test_empty_ring_blocks_consumer() {
    auto q = makeQueue<int, 8>();
    std::atomic<bool> done{false};
    int got = 0;
    std::thread consumer([&] {
        got = q->dequeue();
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_CTX(!done.load(std::memory_order_acquire), "dequeue on an empty ring returned");
    q->enqueue(int(77));
    consumer.join();
    CHECK_CTX(got == 77, "expected 77 got " << got);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Tiny rings under real concurrency. SIZE=1 forces strict lockstep (every
//    item is a full handoff in both directions); SIZE=2 makes the cursors
//    wrap on every other item. Both maximise time spent at the boundary.
// ─────────────────────────────────────────────────────────────────────────────
static void test_lockstep_tiny_rings() {
    streamInOrder<1>(scaled(200'000));
    streamInOrder<2>(scaled(200'000));
    streamInOrder<3>(scaled(200'000));   // non-power-of-two: the ring has no mask
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Sustained throughput-shaped stream on a realistic ring: strict FIFO over
//    millions of items, so any ordering slip eventually shows up.
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_fifo() {
    streamInOrder<1024>(scaled(5'000'000));
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Torn / stale payloads. Each payload fills every word with the same
//    value; if the consumer can observe the ticket before the data write is
//    visible (missing release/acquire), it sees a mix of old and new words.
//    Two sizes: one that fits the 128-byte slot alongside the ticket, and one
//    that makes the slot span multiple 128-byte blocks.
// ─────────────────────────────────────────────────────────────────────────────
template <std::size_t WORDS>
struct Stamp {
    uint64_t w[WORDS];
    Stamp() { std::memset(w, 0, sizeof(w)); }
    explicit Stamp(uint64_t v) { for (auto& x : w) x = v; }
};

template <std::size_t WORDS>
static void tornCheck(uint64_t n) {
    auto q = makeQueue<Stamp<WORDS>, 8>();
    std::thread consumer([&] {
        for (uint64_t i = 1; i <= n; ++i) {
            Stamp<WORDS> s = q->dequeue();
            bool uniform = true;
            for (std::size_t k = 1; k < WORDS; ++k) uniform &= s.w[k] == s.w[0];
            CHECK_CTX(uniform, WORDS * 8 << "B payload torn at item " << i);
            CHECK_CTX(s.w[0] == i, WORDS * 8 << "B payload: expected " << i << " got " << s.w[0]);
            g_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (uint64_t i = 1; i <= n; ++i) {
        q->enqueue(Stamp<WORDS>(i));
        g_produced.fetch_add(1, std::memory_order_relaxed);
    }
    consumer.join();
}

static void test_torn_payloads() {
    static_assert(sizeof(B::spscSlot<Stamp<15>>) == 128, "120B payload fits one slot");
    static_assert(sizeof(B::spscSlot<Stamp<32>>) == 384, "256B payload spans 3 blocks");
    tornCheck<15>(scaled(500'000));
    tornCheck<32>(scaled(500'000));
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Lifetime canary through both enqueue paths. enqueue(A&&) move-assigns
//    over the slot's existing object; emplaceEnqueue destroys it and
//    placement-news a new one. Counted detects destroying a dead object and
//    assigning onto one, and at the end (after the ring's own destructor)
//    constructions must equal destructions exactly.
// ─────────────────────────────────────────────────────────────────────────────
struct Counted {
    static std::atomic<long> ctors, dtors, doubleDestroys, deadAssigns;
    static constexpr uint32_t LIVE = 0xC0FFEE11u, DEAD = 0xDEADBEEFu;
    uint32_t magic;
    int32_t v;

    Counted() : magic(LIVE), v(-1) { ctors.fetch_add(1, std::memory_order_relaxed); }
    explicit Counted(int32_t x) : magic(LIVE), v(x) { ctors.fetch_add(1, std::memory_order_relaxed); }
    Counted(Counted&& o) noexcept : magic(LIVE), v(o.v) { ctors.fetch_add(1, std::memory_order_relaxed); }
    Counted& operator=(Counted&& o) noexcept {
        if (magic != LIVE) deadAssigns.fetch_add(1, std::memory_order_relaxed);
        v = o.v;
        return *this;
    }
    Counted(const Counted&) = delete;             // the queue must never copy
    Counted& operator=(const Counted&) = delete;
    ~Counted() {
        if (magic != LIVE) doubleDestroys.fetch_add(1, std::memory_order_relaxed);
        magic = DEAD;
        dtors.fetch_add(1, std::memory_order_relaxed);
    }
};
std::atomic<long> Counted::ctors{0}, Counted::dtors{0},
                  Counted::doubleDestroys{0}, Counted::deadAssigns{0};

static void test_lifetime_both_enqueue_paths() {
    const uint64_t n = scaled(300'000);
    {
        auto q = makeQueue<Counted, 16>();
        std::thread consumer([&] {
            for (uint64_t i = 0; i < n; ++i) {
                Counted c = q->dequeue();
                CHECK_CTX(c.magic == Counted::LIVE, "dequeued a dead object");
                CHECK_CTX(c.v == int32_t(i), "expected " << i << " got " << c.v);
                g_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (uint64_t i = 0; i < n; ++i) {
            // Alternate in runs of 3 so each slot sees both paths over time.
            if ((i / 3) % 2) q->emplaceEnqueue(int32_t(i));
            else             q->enqueue(Counted(int32_t(i)));
            g_produced.fetch_add(1, std::memory_order_relaxed);
        }
        consumer.join();
    }
    CHECK_CTX(Counted::doubleDestroys.load() == 0,
              Counted::doubleDestroys.load() << " objects destroyed twice");
    CHECK_CTX(Counted::deadAssigns.load() == 0,
              Counted::deadAssigns.load() << " move-assignments onto a destroyed object");
    CHECK_CTX(Counted::ctors.load() == Counted::dtors.load(),
              "ctors " << Counted::ctors.load() << " != dtors " << Counted::dtors.load()
              << " — leaked or double-freed objects");
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Move-only ownership: unique_ptr through both paths. Every pointer must
//    arrive exactly once and be freed exactly once — under ASan a lost or
//    duplicated handoff shows up as a leak or double free.
// ─────────────────────────────────────────────────────────────────────────────
static void test_move_only_ownership() {
    const uint64_t n = scaled(300'000);
    uint64_t sum = 0;
    {
        auto q = makeQueue<std::unique_ptr<uint64_t>, 32>();
        std::thread consumer([&] {
            for (uint64_t i = 0; i < n; ++i) {
                std::unique_ptr<uint64_t> p = q->dequeue();
                CHECK_CTX(p != nullptr, "dequeued an empty unique_ptr at item " << i);
                if (p) sum += *p;
                g_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (uint64_t i = 1; i <= n; ++i) {
            if (i & 1) q->enqueue(std::make_unique<uint64_t>(i));
            else       q->emplaceEnqueue(new uint64_t(i));
            g_produced.fetch_add(1, std::memory_order_relaxed);
        }
        consumer.join();
    }
    CHECK_CTX(sum == n * (n + 1) / 2, "sum " << sum << " expected " << n * (n + 1) / 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Ping-pong through two rings: each side alternates producer and consumer
//    roles on different queues, so every handoff wakes a waiter. Catches
//    lost-publish bugs that a free-running stream can mask with slack.
// ─────────────────────────────────────────────────────────────────────────────
static void test_pingpong() {
    const uint64_t n = scaled(200'000);
    auto req = makeQueue<uint64_t, 2>();
    auto rep = makeQueue<uint64_t, 2>();
    std::thread echo([&] {
        for (uint64_t i = 0; i < n; ++i) rep->enqueue(req->dequeue() + 1);
    });
    for (uint64_t i = 0; i < n; ++i) {
        req->enqueue(uint64_t(i * 2));
        uint64_t v = rep->dequeue();
        CHECK_CTX(v == i * 2 + 1, "round " << i << ": expected " << i * 2 + 1 << " got " << v);
        g_consumed.fetch_add(1, std::memory_order_relaxed);
    }
    echo.join();
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    if (const char* s = std::getenv("ASTRA_STRESS_SCALE")) {
        long v = std::atol(s);
        if (v > 1) g_scale = uint64_t(v);
    }
    std::cout << "spscRingBuffer test suite  (scale: 1/" << g_scale << ")\n" << std::endl;

    std::thread(watchdogLoop).detach();

    run_test("single_thread_fifo_wraparound", 30, "a single-threaded wrap wedged: cursor wrap is broken.", test_single_thread_fifo_wraparound);
    run_test("full_ring_blocks_producer",     30, "producer never unblocked after a slot was freed.", test_full_ring_blocks_producer);
    run_test("empty_ring_blocks_consumer",    30, "consumer never saw a published value.", test_empty_ring_blocks_consumer);
    run_test("lockstep_tiny_rings",           120, "SIZE 1/2/3 handoff wedged at the wrap boundary.", test_lockstep_tiny_rings);
    run_test("concurrent_fifo",               120, "sustained stream wedged.", test_concurrent_fifo);
    run_test("torn_payloads",                 120, "large-payload stream wedged.", test_torn_payloads);
    run_test("lifetime_both_enqueue_paths",   120, "mixed enqueue/emplaceEnqueue stream wedged.", test_lifetime_both_enqueue_paths);
    run_test("move_only_ownership",           120, "unique_ptr stream wedged.", test_move_only_ownership);
    run_test("pingpong",                      120, "cross-ring round trip wedged: a publish was lost.", test_pingpong);

    long f = g_failures.load();
    if (f == 0) {
        std::cout << "\nALL SPSCRINGBUFFER TESTS PASSED" << std::endl;
        return 0;
    }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
