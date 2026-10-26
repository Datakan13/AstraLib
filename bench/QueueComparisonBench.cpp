// ─────────────────────────────────────────────────────────────────────────────
// AtomicRingBuffer vs other concurrent queues.
//
// Competitors (all pinned by tag/commit in bench/CMakeLists.txt):
//
//   MPMC-capable
//     rigtorp::MPMCQueue          Bounded, ticket + per-slot turn counter —
//                                 the same Vyukov-style design as
//                                 AtomicRingBuffer, so this is the closest
//                                 like-for-like comparison. Blocking push/pop.
//     boost::lockfree::queue      Bounded (fixed_sized via capacity<>),
//                                 Michael-Scott linked list over a node pool.
//     moodycamel::ConcurrentQueue UNBOUNDED, per-producer sub-queues; NOT
//                                 FIFO across producers. Different contract,
//                                 included because it's the common default.
//     std::mutex + std::deque     Bounded baseline: what you'd write without
//                                 a lock-free library.
//
//   SPSC-only (run in the 1P/1C configuration only)
//     rigtorp::SPSCQueue
//     moodycamel::ReaderWriterQueue
//     boost::lockfree::spsc_queue
//
// Every bounded queue gets the same capacity (CAP). AtomicRingBuffer and
// rigtorp::MPMCQueue block natively; everything else is wrapped in a
// try-op + _mm_pause retry loop, which is how you'd use them as a blocking
// handoff.
//
// Sections:
//   [1] throughput across producer/consumer shapes (fixed total items)
//   [2] handoff latency: ping-pong round trip between two pinned threads,
//       p50/p99/p99.9. Round trip so both timestamps come from one core's TSC.
//
// Accounting: producers together push exactly the values 1..N; consumers sum
// what they pop. Any loss, duplication or torn value breaks the sum.
// ─────────────────────────────────────────────────────────────────────────────
#include "benchCommon.hpp"

#include <AstraLib/Buffers/atomicRingBuffer.hpp>

#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <concurrentqueue.h>
#include <readerwriterqueue.h>
#include <rigtorp/MPMCQueue.h>
#include <rigtorp/SPSCQueue.h>

#include <deque>
#include <memory>
#include <mutex>

using namespace Bench;

static constexpr std::size_t CAP = 1024;

// ── adapters: push(v) / pop() -> v, both blocking ───────────────────────────
struct AstraQ {
    AstraLib::Buffers::AtomicRingBuffer<uint64_t, CAP> q;
    void push(uint64_t v) { q.enqueue(std::move(v)); }
    uint64_t pop() { return q.dequeue(); }
};

struct RigtorpMPMC {
    rigtorp::MPMCQueue<uint64_t> q{CAP};
    void push(uint64_t v) { q.push(v); }
    uint64_t pop() { uint64_t v; q.pop(v); return v; }
};

struct BoostQueue {
    boost::lockfree::queue<uint64_t, boost::lockfree::capacity<CAP>> q;
    void push(uint64_t v) { while (!q.push(v)) _mm_pause(); }
    uint64_t pop() { uint64_t v; while (!q.pop(v)) _mm_pause(); return v; }
};

struct MoodyCQ {
    moodycamel::ConcurrentQueue<uint64_t> q{CAP};
    void push(uint64_t v) { q.enqueue(v); }   // unbounded: never blocks
    uint64_t pop() { uint64_t v; while (!q.try_dequeue(v)) _mm_pause(); return v; }
};

struct MutexDeque {
    std::mutex m;
    std::deque<uint64_t> d;
    void push(uint64_t v) {
        for (;;) {
            {
                std::lock_guard<std::mutex> g(m);
                if (d.size() < CAP) { d.push_back(v); return; }
            }
            _mm_pause();
        }
    }
    uint64_t pop() {
        for (;;) {
            {
                std::lock_guard<std::mutex> g(m);
                if (!d.empty()) { uint64_t v = d.front(); d.pop_front(); return v; }
            }
            _mm_pause();
        }
    }
};

struct RigtorpSPSC {
    rigtorp::SPSCQueue<uint64_t> q{CAP};
    void push(uint64_t v) { q.push(v); }
    uint64_t pop() {
        uint64_t* p;
        while (!(p = q.front())) _mm_pause();
        uint64_t v = *p;
        q.pop();
        return v;
    }
};

struct MoodyRWQ {
    moodycamel::ReaderWriterQueue<uint64_t> q{CAP};
    void push(uint64_t v) { while (!q.try_enqueue(v)) _mm_pause(); }
    uint64_t pop() { uint64_t v; while (!q.try_dequeue(v)) _mm_pause(); return v; }
};

struct BoostSPSC {
    boost::lockfree::spsc_queue<uint64_t, boost::lockfree::capacity<CAP>> q;
    void push(uint64_t v) { while (!q.push(v)) _mm_pause(); }
    uint64_t pop() { uint64_t v; while (!q.pop(v)) _mm_pause(); return v; }
};

// ── measurements ────────────────────────────────────────────────────────────
template <class Q>
static double throughput(unsigned P, unsigned C, uint64_t N, const char* name) {
    auto q = std::make_unique<Q>();
    std::atomic<uint64_t> sum{0};
    const uint64_t perProducer = N / P, perConsumer = N / C;
    const int64_t ns = runPinned(P + C, [&](unsigned i) {
        if (i < P) {
            for (uint64_t k = 0; k < perProducer; ++k) q->push(i + 1 + k * P);
        } else {
            uint64_t s = 0;
            for (uint64_t k = 0; k < perConsumer; ++k) s += q->pop();
            sum.fetch_add(s, std::memory_order_relaxed);
        }
    });
    const uint64_t expect = N * (N + 1) / 2;
    BENCH_CHECK(sum.load() == expect, "%s %uP/%uC: sum %llu, expected %llu",
                name, P, C, (unsigned long long)sum.load(), (unsigned long long)expect);
    return double(N) * 1e3 / double(ns);   // M ops/s
}

template <class Q>
static std::vector<double> pingPong(uint64_t samples) {
    auto req = std::make_unique<Q>();
    auto rep = std::make_unique<Q>();
    std::vector<double> rtt;
    rtt.reserve(samples);
    runPinned(2, [&](unsigned i) {
        if (i == 1) {
            for (uint64_t k = 0; k < samples; ++k) rep->push(req->pop());
            return;
        }
        for (uint64_t k = 0; k < samples; ++k) {
            const uint64_t t0 = __rdtsc();
            req->push(k + 1);
            const uint64_t v = rep->pop();
            const uint64_t t1 = __rdtsc();
            doNotOptimize(v);
            rtt.push_back(cyclesToNs(t1 - t0));
        }
    });
    std::sort(rtt.begin(), rtt.end());
    return rtt;
}

struct Competitor {
    const char* name;
    const char* note;
    bool spscOnly;
    double (*tput)(unsigned, unsigned, uint64_t, const char*);
    std::vector<double> (*lat)(uint64_t);
};

template <class Q>
static Competitor make(const char* name, const char* note, bool spscOnly) {
    return {name, note, spscOnly, &throughput<Q>, &pingPong<Q>};
}

int main() {
    init();
    printPreamble("AtomicRingBuffer vs concurrent queues  (uint64_t payload, capacity 1024)");

    const std::vector<Competitor> all = {
        make<AstraQ>     ("AstraLib AtomicRingBuffer",   "", false),
        make<RigtorpMPMC>("rigtorp::MPMCQueue",          "", false),
        make<BoostQueue> ("boost::lockfree::queue",      "", false),
        make<MoodyCQ>    ("moodycamel::ConcurrentQueue", "(unbounded, not FIFO)", false),
        make<MutexDeque> ("std::mutex + std::deque",     "", false),
        make<RigtorpSPSC>("rigtorp::SPSCQueue",          "(SPSC only)", true),
        make<MoodyRWQ>   ("moodycamel::ReaderWriterQ",   "(SPSC only)", true),
        make<BoostSPSC>  ("boost::lockfree::spsc_queue", "(SPSC only)", true),
    };

    std::printf("\n[1] Throughput, M ops/s (higher is better)\n");
    struct Shape { unsigned P, C; uint64_t base; };
    const Shape shapes[] = {{1, 1, 4'000'000}, {2, 2, 4'000'000},
                            {4, 4, 4'000'000}, {4, 1, 2'000'000}, {1, 4, 2'000'000}};
    for (const Shape& s : shapes) {
        char title[96];
        std::snprintf(title, sizeof(title), "%uP/%uC", s.P, s.C);
        // +1: the coordinator gets its own core so its join never steals one.
        if (s.P + s.C + 1 > coreCount()) {
            std::printf("\n  %s  SKIPPED — needs %u physical cores, have %u\n",
                        title, s.P + s.C + 1, coreCount());
            continue;
        }
        uint64_t N = scaled(s.base);
        N -= N % (uint64_t(s.P) * s.C);

        const bool spsc = s.P == 1 && s.C == 1;
        std::vector<std::function<double()>> fns;
        std::vector<std::string> names;
        std::vector<const Competitor*> used;
        for (const Competitor& c : all) {
            if (c.spscOnly && !spsc) continue;
            used.push_back(&c);
            names.push_back(c.name);
            fns.push_back([&c, &s, N] { return c.tput(s.P, s.C, N, c.name); });
        }
        Deadline dl(title, 600);
        const std::vector<Stat> st = interleaved(fns, names);
        std::vector<Row> rows;
        for (size_t i = 0; i < used.size(); ++i)
            rows.push_back({used[i]->name, used[i]->note, st[i]});
        printTable(title, "M ops/s", rows);
    }

    std::printf("\n[2] Handoff latency, ping-pong round trip in ns (lower is better)\n");
    if (coreCount() < 3) {
        std::printf("  SKIPPED — needs 3 physical cores\n");
    } else {
        const uint64_t samples = scaled(200'000);
        std::printf("    %-30s %9s %9s %9s %10s\n", "", "p50", "p99", "p99.9", "max");
        for (const Competitor& c : all) {
            Deadline dl(c.name, 300);
            c.lat(samples / 10);                       // warmup
            std::vector<double> r = c.lat(samples);
            std::printf("    %-30s %9.0f %9.0f %9.0f %10.0f\n", c.name,
                        percentile(r, 0.50), percentile(r, 0.99),
                        percentile(r, 0.999), r.back());
        }
    }
    return finish();
}
