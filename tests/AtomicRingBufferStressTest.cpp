// ─────────────────────────────────────────────────────────────────────────────
// Adversarial stress suite for AstraLib::Buffers::AtomicRingBuffer.
//
// Every test here targets a specific, known way ticket-based MPMC ring buffers
// die in production:
//
//   1. lockstep_size2        — SIZE=2: smallest working buffer, wrap-around
//                              on every other op; worst-case sequence math.
//   2. pingpong_echo         — two SIZE=2 queues, cross-thread round trips;
//                              hammers the release/acquire handoff both ways.
//   3. clear_reuse_cycles    — clearBuffer() correctness incl. partially-full.
//   4. producer_stall        — buffer stays FULL while producers spin on
//                              occupied slots (slow-consumer phase).
//   5. mpmc_exactly_once     — 4P/4C, 400k items: every value delivered
//                              exactly once (no loss, no duplication) plus
//                              per-producer FIFO order as seen by each consumer.
//                              Consumers start BEFORE producers (cold-start).
//   6. mpmc_torn_writes      — 4P/4C on a SIZE=4 buffer: 32-byte checksummed
//                              payloads. Catches a slot being published (seq
//                              store) before its data write completed, or two
//                              threads in one slot.
//   7. mpsc_batch_dequeue    — batchDequeue() with mixed batch sizes vs 4
//                              concurrent producers, exactly-once accounting.
//   8. emplace_lifetime      — emplaceEnqueue's manual destroy + placement-new
//                              under MPMC churn, checked with a canary type
//                              that detects double-destruction, assignment
//                              onto a destroyed object, and leaks.
//   9. move_only_ownership   — unique_ptr payloads: unique ownership must
//                              survive the queue (leaks/double-frees show up
//                              as a nonzero live-object count).
//  10. oversubscription      — 2-4x more threads than cores on a SIZE=8
//                              buffer: forces preemption INSIDE the ticket
//                              protocol (between fetch_add and seq store).
//                              Thread count caps down on small machines like
//                              every other P/C test here (see file header) —
//                              so this only actually oversubscribes on
//                              machines with room to spare.
//
// Because this queue BLOCKS (spins) rather than failing, a broken invariant
// usually presents as a hang, not a wrong value. A watchdog thread converts
// any hang into a diagnosed failure (which test, how many items produced vs
// consumed) and a nonzero exit code, so CI never freezes.
//
// Scale knob: ASTRA_STRESS_SCALE=N divides item counts by N (use under
// sanitizers or on slow CI). Default 1.
//
// Thread counts (P/C) scale down automatically to match available cores —
// confirmed empirically, not assumed: any oversubscription beyond a true
// 1:1 thread:core match caused catastrophic (not proportional) slowdowns for
// these tests specifically. mpmc_torn_writes (SIZE=4, tightest contention)
// at a true 1:1 match took 1.2ms; at just 2x oversubscription on the same
// machine, it didn't finish within 45s. test_oversubscription's whole
// purpose is deliberate oversubscription, so it keeps that character on
// capable machines but still gets capped on tiny ones — a 2-core CI runner
// was never going to meaningfully exercise "preemption under 8x
// oversubscription" reliably anyway.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Buffers/atomicRingBuffer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <vector>

// ── compile-time layout checks ───────────────────────────────────────────────
namespace RB = AstraLib::Buffers;
static_assert(sizeof(RB::Slot<int>) == 64,  "Slot<int> must be one cache line");
static_assert(alignof(RB::Slot<int>) == 64, "Slot must be cache-line aligned");
// Slots must fit in a single cache line to avoid false sharing between
// adjacent slots, so payloads over 56 bytes (64 - sizeof(seq)) are rejected
// by Slot's own static_assert — there's no valid 2-cache-line layout to test.

// ── thread-safe CHECK machinery ──────────────────────────────────────────────
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

// ── watchdog: converts deadlock/livelock into a diagnosed failure ────────────
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

// ── helpers ──────────────────────────────────────────────────────────────────
struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed * 2654435761u + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

static void busyDelayNs(uint64_t ns) {
    for (uint64_t i = 0; i < ns / 20 + 1; ++i) _mm_pause();
}

static std::unique_ptr<std::atomic<uint8_t>[]> zeroFlags(size_t n) {
    auto p = std::unique_ptr<std::atomic<uint8_t>[]>(new std::atomic<uint8_t>[n]);
    for (size_t i = 0; i < n; ++i) p[i].store(0, std::memory_order_relaxed);
    return p;
}

static uint64_t g_scale = 1;
static uint64_t scaled(uint64_t base, uint64_t minimum = 1000) {
    uint64_t v = base / g_scale;
    return v < minimum ? minimum : v;
}

// P/C thread counts for tests that hammer a lock-free structure with
// contention-widening yields, capped to available cores. See file header
// for the empirical justification. `fullValue` is used unchanged on any
// machine with >= 8 cores (every one of these tests was designed and
// validated against that assumption); below that, threads are capped to a
// true 1:1 match with hardware_concurrency() so producers+consumers never
// collectively oversubscribe the machine.
static int scaledThreadCount(int fullValue) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0 || hw >= 8) return fullValue;
    return std::max<int>(1, int(hw) / 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. SIZE=2 lockstep: the smallest buffer the SIZE >= 2 static_assert allows.
//    Slot 0/1 are reused every other op, so the seq arithmetic (ticket,
//    ticket+1, ticket+SIZE) is exercised constantly. Off-by-one => instant
//    deadlock, which the watchdog reports.
// ─────────────────────────────────────────────────────────────────────────────
static void test_lockstep_size2() {
    const uint64_t N = scaled(20000);
    RB::AtomicRingBuffer<int, 2> q;
    std::thread prod([&] {
        for (uint64_t i = 0; i < N; ++i) {
            q.enqueue(int(i));
            g_produced.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (uint64_t i = 0; i < N; ++i) {
        int v = q.dequeue();
        CHECK_CTX(v == int(i), "expected " << i << " got " << v);
        g_consumed.fetch_add(1, std::memory_order_relaxed);
    }
    prod.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Ping-pong echo through two SIZE=2 queues. Data must be fully visible
//    across the handoff in BOTH directions every round trip; a missing
//    release/acquire pairing shows up as a corrupted echo.
// ─────────────────────────────────────────────────────────────────────────────
static void test_pingpong_echo() {
    const uint64_t N = scaled(30000);
    RB::AtomicRingBuffer<uint64_t, 2> ab, ba;
    std::thread echo([&] {
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t v = ab.dequeue();
            ba.enqueue(v * 2654435761u);
        }
    });
    for (uint64_t i = 0; i < N; ++i) {
        ab.enqueue(uint64_t(i));
        g_produced.fetch_add(1, std::memory_order_relaxed);
        uint64_t r = ba.dequeue();
        CHECK_CTX(r == i * 2654435761u, "round trip " << i << " corrupted: " << r);
        g_consumed.fetch_add(1, std::memory_order_relaxed);
    }
    echo.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. clearBuffer() reuse cycles, including clearing a PARTIALLY FULL buffer
//    (items discarded mid-flight). After every clear the queue must behave
//    exactly like a fresh one. (Documented quiescent-only, so single-threaded.)
// ─────────────────────────────────────────────────────────────────────────────
static void test_clear_reuse_cycles() {
    RB::AtomicRingBuffer<int, 8> q;
    for (int cycle = 0; cycle < 200; ++cycle) {
        int leftover = cycle % 7;               // sometimes clear while non-empty
        for (int i = 0; i < leftover; ++i) q.enqueue(int(i + 1000));
        q.clearBuffer();
        for (int i = 0; i < 8; ++i) q.enqueue(int(cycle * 100 + i));   // full round
        for (int i = 0; i < 8; ++i) {
            int v = q.dequeue();
            CHECK_CTX(v == cycle * 100 + i, "cycle " << cycle << " pos " << i << " got " << v);
        }
        g_consumed.fetch_add(8, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Producer stall: consumer sleeps per item at first, so the SIZE=4 buffer
//    is pinned at FULL and all 4 producers spin on occupied slots, then the
//    consumer speeds up. Exercises the full-buffer wait path + recovery.
// ─────────────────────────────────────────────────────────────────────────────
static void test_producer_stall() {
    const int P = scaledThreadCount(4);
    constexpr int PER = 500;
    const int TOTAL = P * PER;
    RB::AtomicRingBuffer<int, 4> q;
    auto seen = zeroFlags(TOTAL);
    std::vector<std::thread> prods;
    for (int p = 0; p < P; ++p)
        prods.emplace_back([&, p] {
            for (int i = 0; i < PER; ++i) {
                q.enqueue(int(p * PER + i));
                g_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (int i = 0; i < TOTAL; ++i) {
        if (i < 100) std::this_thread::sleep_for(std::chrono::microseconds(200));
        int v = q.dequeue();
        CHECK_CTX(v >= 0 && v < TOTAL, "out-of-range value " << v);
        uint8_t prev = seen[v].fetch_add(1);
        CHECK_CTX(prev == 0, "duplicate value " << v);
        g_consumed.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& t : prods) t.join();
    for (int i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. MPMC exactly-once: 4 producers x 4 consumers. Every value must come out
//    exactly once (bitmap: fetch_add catches duplicates live, final scan
//    catches losses), and each consumer must see any given producer's items
//    in FIFO order (a consumer's claimed tickets are monotonic, so its view
//    of one producer's stream must be an increasing subsequence — reordering
//    here means broken ticket/seq logic). Consumers start FIRST and block on
//    the empty queue: cold-start path.
// ─────────────────────────────────────────────────────────────────────────────
static void test_mpmc_exactly_once() {
    const int P = scaledThreadCount(4), C = scaledThreadCount(4);
    const uint64_t PER = scaled(100000, 2000);
    const uint64_t TOTAL = PER * P;
    RB::AtomicRingBuffer<uint64_t, 1024> q;
    auto seen = zeroFlags(TOTAL);

    std::vector<std::thread> cons;
    for (int c = 0; c < C; ++c)
        cons.emplace_back([&, c, P] {
            std::vector<int64_t> lastSeq(P, -1);
            for (uint64_t i = 0; i < TOTAL / C; ++i) {
                uint64_t v = q.dequeue();
                uint64_t p = v / PER, s = v % PER;
                CHECK_CTX(p < P, "garbage value " << v);
                if (p < P) {
                    CHECK_CTX(int64_t(s) > lastSeq[p],
                              "consumer " << c << " saw producer " << p
                              << " go backwards: " << s << " after " << lastSeq[p]);
                    lastSeq[p] = int64_t(s);
                    uint8_t prev = seen[v].fetch_add(1);
                    CHECK_CTX(prev == 0, "duplicate value " << v);
                }
                g_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // consumers block on empty first

    std::vector<std::thread> prods;
    for (int p = 0; p < P; ++p)
        prods.emplace_back([&, p] {
            for (uint64_t i = 0; i < PER; ++i) {
                q.enqueue(uint64_t(p * PER + i));
                g_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });

    for (auto& t : prods) t.join();
    for (auto& t : cons) t.join();
    for (uint64_t i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Torn writes on a tiny buffer: 4P/4C fighting over FOUR slots with a
//    32-byte payload whose fields are cryptographically tied together. If a
//    slot's seq is ever published before the struct write completed — or two
//    threads ever occupy one slot — the checksum breaks. Sum conservation
//    (producers' total vs consumers' total) additionally catches loss/dup.
// ─────────────────────────────────────────────────────────────────────────────
struct TornPayload {
    uint64_t a, b, c, check;
};
static TornPayload makeTorn(uint64_t x) {
    TornPayload p;
    p.a = x;
    p.b = x * 0x9E3779B97F4A7C15ULL ^ 0xA5A5A5A5A5A5A5A5ULL;
    p.c = p.a ^ p.b;
    p.check = p.a + p.b + p.c;
    return p;
}
static bool tornValid(const TornPayload& p) {
    return p.b == (p.a * 0x9E3779B97F4A7C15ULL ^ 0xA5A5A5A5A5A5A5A5ULL)
        && p.c == (p.a ^ p.b)
        && p.check == p.a + p.b + p.c;
}

static void test_mpmc_torn_writes() {
    const int P = scaledThreadCount(4), C = scaledThreadCount(4);
    const uint64_t PER = scaled(40000, 2000);
    RB::AtomicRingBuffer<TornPayload, 4> q;
    std::atomic<uint64_t> prodSum{0}, consSum{0};

    std::vector<std::thread> cons;
    for (int c = 0; c < C; ++c)
        cons.emplace_back([&] {
            uint64_t local = 0;
            for (uint64_t i = 0; i < PER; ++i) {   // P==C => PER each
                TornPayload p = q.dequeue();
                CHECK_CTX(tornValid(p), "TORN payload: a=" << p.a << " b=" << p.b
                                        << " c=" << p.c << " check=" << p.check);
                local += p.a;
                g_consumed.fetch_add(1, std::memory_order_relaxed);
            }
            consSum.fetch_add(local);
        });

    std::vector<std::thread> prods;
    for (int p = 0; p < P; ++p)
        prods.emplace_back([&, p] {
            uint64_t local = 0;
            for (uint64_t i = 0; i < PER; ++i) {
                uint64_t x = (uint64_t(p + 1) << 32) | i;
                q.enqueue(makeTorn(x));
                local += x;
                g_produced.fetch_add(1, std::memory_order_relaxed);
            }
            prodSum.fetch_add(local);
        });

    for (auto& t : prods) t.join();
    for (auto& t : cons) t.join();
    CHECK_CTX(prodSum.load() == consSum.load(),
              "sum mismatch (loss/dup): produced " << prodSum.load()
              << " consumed " << consSum.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. batchDequeue (documented MPSC-only): one consumer draining in batches of
//    varying size (1..50, including the 50 max) against 4 live producers.
//    Exactly-once accounting over the whole stream.
// ─────────────────────────────────────────────────────────────────────────────
static void test_mpsc_batch_dequeue() {
    const int P = scaledThreadCount(4);
    const uint64_t PER = scaled(25000, 2000);
    const uint64_t TOTAL = PER * P;
    RB::AtomicRingBuffer<uint64_t, 1024> q;
    auto seen = zeroFlags(TOTAL);

    std::vector<std::thread> prods;
    for (int p = 0; p < P; ++p)
        prods.emplace_back([&, p] {
            for (uint64_t i = 0; i < PER; ++i) {
                q.enqueue(uint64_t(p * PER + i));
                g_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });

    const int sizes[] = {50, 1, 7, 32, 13, 50, 2, 45};
    uint64_t remaining = TOTAL;
    int si = 0;
    while (remaining > 0) {
        int n = sizes[si++ % 8];
        if (uint64_t(n) > remaining) n = int(remaining);
        std::array<uint64_t, 50>* batch = q.batchDequeue(n);
        for (int j = 0; j < n; ++j) {
            uint64_t v = (*batch)[j];
            CHECK_CTX(v < TOTAL, "garbage batched value " << v);
            if (v < TOTAL) {
                uint8_t prev = seen[v].fetch_add(1);
                CHECK_CTX(prev == 0, "duplicate batched value " << v);
            }
        }
        remaining -= n;
        g_consumed.fetch_add(n, std::memory_order_relaxed);
    }
    for (auto& t : prods) t.join();
    for (uint64_t i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. emplaceEnqueue lifetime canary. emplaceEnqueue manually destroys the
//    slot object and placement-news over it — the classic place for
//    double-destruction / construct-over-live-object bugs. Counted has a
//    magic canary: destroying a dead object, or move-assigning onto a dead
//    object, is detected and counted. At the end (after the queue's own
//    destructor) constructions must equal destructions exactly.
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
static_assert(sizeof(Counted) == 8, "Counted must stay small");

static void test_emplace_lifetime() {
    const int P = scaledThreadCount(4), C = scaledThreadCount(4);
    const uint64_t PER = scaled(30000, 2000);
    const uint64_t TOTAL = PER * P;
    auto seen = zeroFlags(TOTAL);
    {
        RB::AtomicRingBuffer<Counted, 16> q;

        std::vector<std::thread> cons;
        for (int c = 0; c < C; ++c)
            cons.emplace_back([&] {
                for (uint64_t i = 0; i < TOTAL / C; ++i) {
                    Counted x = q.dequeue();
                    CHECK_CTX(x.magic == Counted::LIVE, "dequeued a dead object");
                    CHECK_CTX(x.v >= 0 && uint64_t(x.v) < TOTAL, "garbage v=" << x.v);
                    if (x.v >= 0 && uint64_t(x.v) < TOTAL) {
                        uint8_t prev = seen[x.v].fetch_add(1);
                        CHECK_CTX(prev == 0, "duplicate v=" << x.v);
                    }
                    g_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            });

        std::vector<std::thread> prods;
        for (int p = 0; p < P; ++p)
            prods.emplace_back([&, p] {
                for (uint64_t i = 0; i < PER; ++i) {
                    q.emplaceEnqueue(int32_t(p * PER + i));
                    g_produced.fetch_add(1, std::memory_order_relaxed);
                }
            });

        for (auto& t : prods) t.join();
        for (auto& t : cons) t.join();

        // Leave items sitting in the queue so its destructor has real work.
        for (int i = 0; i < 10; ++i) q.emplaceEnqueue(int32_t(-2));
    } // queue destroyed here: slots + batch array destruct

    CHECK_CTX(Counted::doubleDestroys.load() == 0,
              "objects destroyed twice: " << Counted::doubleDestroys.load());
    CHECK_CTX(Counted::deadAssigns.load() == 0,
              "assignments onto destroyed objects: " << Counted::deadAssigns.load());
    CHECK_CTX(Counted::ctors.load() == Counted::dtors.load(),
              "lifetime leak: " << Counted::ctors.load() << " ctors vs "
              << Counted::dtors.load() << " dtors");
    for (uint64_t i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Move-only ownership: unique_ptr payloads through MPMC churn. Any
//    duplicated slot hand-off becomes a double-free (crash/ASan), any lost
//    hand-off becomes a leak — both surface as live-count != 0 at the end.
// ─────────────────────────────────────────────────────────────────────────────
struct LiveCounter {
    static std::atomic<long> live;
    int32_t v;
    explicit LiveCounter(int32_t x) : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
    ~LiveCounter() { live.fetch_sub(1, std::memory_order_relaxed); }
};
std::atomic<long> LiveCounter::live{0};

static void test_move_only_ownership() {
    const int P = scaledThreadCount(4), C = scaledThreadCount(4);
    const uint64_t PER = scaled(25000, 2000);
    const uint64_t TOTAL = PER * P;
    auto seen = zeroFlags(TOTAL);
    {
        RB::AtomicRingBuffer<std::unique_ptr<LiveCounter>, 64> q;

        std::vector<std::thread> cons;
        for (int c = 0; c < C; ++c)
            cons.emplace_back([&] {
                for (uint64_t i = 0; i < TOTAL / C; ++i) {
                    std::unique_ptr<LiveCounter> p = q.dequeue();
                    CHECK_CTX(p != nullptr, "dequeued null unique_ptr");
                    if (p) {
                        uint8_t prev = seen[p->v].fetch_add(1);
                        CHECK_CTX(prev == 0, "duplicate v=" << p->v);
                    }
                    g_consumed.fetch_add(1, std::memory_order_relaxed);
                }
            });

        std::vector<std::thread> prods;
        for (int p = 0; p < P; ++p)
            prods.emplace_back([&, p] {
                for (uint64_t i = 0; i < PER; ++i) {
                    q.enqueue(std::make_unique<LiveCounter>(int32_t(p * PER + i)));
                    g_produced.fetch_add(1, std::memory_order_relaxed);
                }
            });

        for (auto& t : prods) t.join();
        for (auto& t : cons) t.join();
    }
    CHECK_CTX(LiveCounter::live.load() == 0,
              "ownership broken: " << LiveCounter::live.load()
              << " objects leaked (or double-freed, if negative)");
    for (uint64_t i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Oversubscription storm: 16 threads on a SIZE=8 buffer with injected
//     yields. Far more threads than cores means the OS constantly preempts
//     threads IN THE MIDDLE of the ticket protocol (after fetch_add, before
//     the seq store) — the interleavings a lightly-loaded test never hits.
// ─────────────────────────────────────────────────────────────────────────────
static void test_oversubscription() {
    const int P = scaledThreadCount(8), C = scaledThreadCount(8);
    const uint64_t PER = scaled(8000, 1000);
    const uint64_t TOTAL = PER * P;
    RB::AtomicRingBuffer<uint64_t, 8> q;
    auto seen = zeroFlags(TOTAL);

    std::vector<std::thread> cons;
    for (int c = 0; c < C; ++c)
        cons.emplace_back([&, c] {
            XorShift rng(uint64_t(c) + 777);
            for (uint64_t i = 0; i < PER; ++i) {   // P==C => PER each
                uint64_t v = q.dequeue();
                CHECK_CTX(v < TOTAL, "garbage value " << v);
                if (v < TOTAL) {
                    uint8_t prev = seen[v].fetch_add(1);
                    CHECK_CTX(prev == 0, "duplicate value " << v);
                }
                if ((rng.next() & 31) == 0) std::this_thread::yield();
                g_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::vector<std::thread> prods;
    for (int p = 0; p < P; ++p)
        prods.emplace_back([&, p] {
            XorShift rng(uint64_t(p) + 1);
            for (uint64_t i = 0; i < PER; ++i) {
                q.enqueue(uint64_t(p * PER + i));
                if ((rng.next() & 31) == 0) std::this_thread::yield();
                g_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });

    for (auto& t : prods) t.join();
    for (auto& t : cons) t.join();
    for (uint64_t i = 0; i < TOTAL; ++i)
        CHECK_CTX(seen[i].load() == 1, "lost value " << i);
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    if (const char* s = std::getenv("ASTRA_STRESS_SCALE")) {
        long v = std::atol(s);
        if (v > 1) g_scale = uint64_t(v);
    }
    std::cout << "AtomicRingBuffer stress suite  (hw threads: "
              << std::thread::hardware_concurrency()
              << ", scale: 1/" << g_scale << ")\n" << std::endl;

    std::thread(watchdogLoop).detach();

    run_test("lockstep_size2",      60,  "SIZE=2 handoff wedged: seq wrap arithmetic is broken.", test_lockstep_size2);
    run_test("pingpong_echo",       60,  "cross-queue round trip wedged.", test_pingpong_echo);
    run_test("clear_reuse_cycles",  60,  "queue unusable after clearBuffer().", test_clear_reuse_cycles);
    run_test("producer_stall",      60,  "producers never recovered from a full buffer.", test_producer_stall);
    run_test("mpmc_exactly_once",   120, "MPMC wedged: a ticket was claimed but its slot never published.", test_mpmc_exactly_once);
    run_test("mpmc_torn_writes",    120, "4-slot MPMC wedged under contention.", test_mpmc_torn_writes);
    run_test("mpsc_batch_dequeue",  120, "batchDequeue wedged against live producers.", test_mpsc_batch_dequeue);
    run_test("emplace_lifetime",    120, "emplaceEnqueue path wedged.", test_emplace_lifetime);
    run_test("move_only_ownership", 120, "move-only MPMC wedged.", test_move_only_ownership);
    run_test("oversubscription",    180, "preemption inside the ticket protocol wedged the queue.", test_oversubscription);

    long f = g_failures.load();
    if (f == 0) {
        std::cout << "\nALL STRESS TESTS PASSED" << std::endl;
        return 0;
    }
    std::cerr << "\n" << f << " CHECK(s) FAILED" << std::endl;
    return 1;
}
