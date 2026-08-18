// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Atomic::PaddedAtomic<T>.
//
// PaddedAtomic exists to give each atomic its own cache line, so the
// property actually worth testing isn't "atomics work" (the standard
// library already guarantees that) — it's that the padding does its job:
// every element of an array of PaddedAtomic must be independently
// addressable at a 64-byte stride, and concurrent traffic on one element
// must never perturb its neighbor's bit pattern. A padding-size miscalculation
// (e.g. from a T where sizeof(std::atomic<T>) doesn't divide evenly, or a
// future edit that shrinks the padding array) would show up exactly as
// cross-talk between adjacent elements — which is what these tests target.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Atomic/paddedAtomic.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

static long g_failures = 0;
static std::atomic<long> g_failuresAtomic{0};

#define CHECK_CTX(cond, ctx)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__            \
                      << "  CHECK(" #cond ")  " << ctx << "\n";              \
        }                                                                    \
    } while (0)
#define CHECK(cond) CHECK_CTX(cond, "")

#define CHECK_CTX_MT(cond, ctx)                                              \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_failuresAtomic.fetch_add(1, std::memory_order_relaxed);        \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__            \
                      << "  CHECK(" #cond ")  " << ctx << "\n";              \
        }                                                                    \
    } while (0)

using AstraLib::Atomic::PaddedAtomic;

// ── layout: the header already static_asserts sizeof(PaddedAtomic<int>)==64;
//    extend that to other common payload types so a future edit that breaks
//    layout for, say, int64_t or a pointer doesn't slip through unnoticed ──
static_assert(sizeof(PaddedAtomic<bool>)    == 64, "PaddedAtomic<bool> must be 64 bytes");
static_assert(sizeof(PaddedAtomic<int64_t>) == 64, "PaddedAtomic<int64_t> must be 64 bytes");
static_assert(sizeof(PaddedAtomic<void*>)   == 64, "PaddedAtomic<void*> must be 64 bytes");
static_assert(alignof(PaddedAtomic<int>)    == 64, "PaddedAtomic<int> must be 64-byte aligned");

// ─────────────────────────────────────────────────────────────────────────────
// 1. Value semantics: default ctor and value ctor actually store what's given.
// ─────────────────────────────────────────────────────────────────────────────
static void test_value_semantics() {
    PaddedAtomic<int> a(42);
    CHECK_CTX(a.value.load() == 42, "value ctor: expected 42 got " << a.value.load());

    a.value.store(7);
    CHECK_CTX(a.value.load() == 7, "store/load round trip: expected 7 got " << a.value.load());

    PaddedAtomic<int64_t> b(int64_t(-123456789));
    CHECK_CTX(b.value.load() == -123456789,
              "int64_t value ctor: got " << b.value.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Array stride: consecutive elements must sit exactly 64 bytes apart, with
//    zero overlap between one element's [value, padding] region and the next.
// ─────────────────────────────────────────────────────────────────────────────
static void test_array_stride_no_overlap() {
    constexpr int N = 8;
    PaddedAtomic<int> arr[N];
    for (int i = 0; i < N - 1; ++i) {
        auto* p0 = reinterpret_cast<const char*>(&arr[i]);
        auto* p1 = reinterpret_cast<const char*>(&arr[i + 1]);
        CHECK_CTX(p1 - p0 == 64, "arr[" << i << "] to arr[" << (i+1)
                  << "] stride is " << (p1 - p0) << ", expected 64");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Cross-talk under concurrency: N threads each hammer a DIFFERENT element
//    of a PaddedAtomic<int> array with fetch_add, while occasionally reading
//    every OTHER element to confirm it only ever holds a value its own owner
//    thread could have produced. Any bleed between neighbors (a padding
//    miscalculation causing one element's writes to land in another's
//    memory) shows up as a value outside the owner's possible range.
// ─────────────────────────────────────────────────────────────────────────────
static void test_no_cross_talk_between_elements() {
    constexpr int N = 16;
    constexpr int ITERS = 200000;
    PaddedAtomic<int> arr[N];
    for (int i = 0; i < N; ++i) arr[i].value.store(0, std::memory_order_relaxed);

    std::vector<std::thread> ts;
    for (int i = 0; i < N; ++i)
        ts.emplace_back([&, i] {
            for (int j = 0; j < ITERS; ++j) {
                int prev = arr[i].value.fetch_add(1, std::memory_order_acq_rel);
                // Only this thread ever increments arr[i], so it must observe
                // a strictly increasing, gap-free sequence of its own writes.
                CHECK_CTX_MT(prev == j, "element " << i << " expected prior value "
                             << j << " got " << prev << " — a concurrent writer "
                             "reached into a neighboring element's memory");
            }
        });
    for (auto& t : ts) t.join();

    for (int i = 0; i < N; ++i)
        CHECK_CTX_MT(arr[i].value.load() == ITERS,
                     "element " << i << " final value " << arr[i].value.load()
                     << ", expected " << ITERS);
}

int main() {
    std::cout << "PaddedAtomic test suite\n" << std::endl;

    std::cout << "[ RUN  ] value_semantics" << std::endl;
    test_value_semantics();
    std::cout << "[ RUN  ] array_stride_no_overlap" << std::endl;
    test_array_stride_no_overlap();
    std::cout << "[ RUN  ] no_cross_talk_between_elements" << std::endl;
    test_no_cross_talk_between_elements();

    long total = g_failures + g_failuresAtomic.load();
    if (total == 0) { std::cout << "\nALL PADDEDATOMIC TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << total << " CHECK(s) FAILED" << std::endl;
    return 1;
}
