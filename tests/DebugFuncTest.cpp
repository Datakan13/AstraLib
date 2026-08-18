// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Debug::debugMessage.
//
// debugMessage only prints (void return, no observable state besides
// stdout), so tests here redirect std::cout's streambuf into a
// std::ostringstream for the duration of a call and assert on the captured
// text — same technique used in TimerTest.cpp.
//
// KNOWN BUG, confirmed both under ThreadSanitizer AND as an actual crash in
// a plain build: debugMessage calls std::localtime(), which per glibc's own
// docs is NOT thread-safe — it returns a pointer into a single shared static
// buffer, and the *result* is copied into a local std::tm only AFTER the
// call returns. Two threads logging "simultaneously" (exactly the use case
// a function that prints its own TID exists for) race on that shared
// buffer. Under `cmake -DENABLE_TSAN=ON`, this file's
// concurrent_calls_do_not_crash test reproduces it directly as a TSan
// data-race report pointing at glibc's tzset_internal/localtime. It did NOT
// crash a plain (non-sanitized) build across 5 isolated manual runs in
// development — but DID segfault this exact test (reported as
// "DebugFuncTest ... Exception: SegFault") in 2 out of 2 full `ctest` suite
// runs. So it's a real crash risk without a sanitizer too, not just a
// theoretical TSan-only finding — apparently reliable enough in a full-suite
// run to not be dismissed as a fluke, even though it didn't show up in
// smaller standalone probes. Not asserted on directly with a CHECK,
// since an intermittent crash can't be pinned to a boolean the test
// controls — the crash IS the failure signal ctest reports. Fix (swap to
// localtime_r, which is reentrant) belongs in the header, not the test.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Debug/debugFunc.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

template <typename Fn>
static std::string captureStdout(Fn&& fn) {
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    fn();
    std::cout.rdbuf(old);
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Output contains the expected markers and the actual arguments passed —
//    not just "doesn't crash".
// ─────────────────────────────────────────────────────────────────────────────
static void test_output_contains_arguments_and_markers() {
    std::string out = captureStdout([] {
        AstraLib::Debug::debugMessage("hello ", 42, " world ", 3.5);
    });
    CHECK_CTX(out.find("[Debug]") != std::string::npos, "missing [Debug] marker: " << out);
    CHECK_CTX(out.find("ThreadID (TID):") != std::string::npos, "missing TID marker: " << out);
    CHECK_CTX(out.find("hello ") != std::string::npos, "missing first argument: " << out);
    CHECK_CTX(out.find("42") != std::string::npos, "missing int argument: " << out);
    CHECK_CTX(out.find("world") != std::string::npos, "missing string argument: " << out);
    CHECK_CTX(out.find("3.5") != std::string::npos, "missing double argument: " << out);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Zero-argument and single-argument calls must not crash or malform.
// ─────────────────────────────────────────────────────────────────────────────
static void test_variadic_edge_counts() {
    std::string zero = captureStdout([] { AstraLib::Debug::debugMessage(); });
    CHECK_CTX(zero.find("[Debug]") != std::string::npos, "zero-arg call missing marker: " << zero);

    std::string one = captureStdout([] { AstraLib::Debug::debugMessage("solo"); });
    CHECK_CTX(one.find("solo") != std::string::npos, "single-arg call missing its argument: " << one);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Concurrency smoke test: many threads calling debugMessage at once must
//    not crash the process (see file header re: the localtime race this
//    does NOT reliably crash on outside of TSan). No content assertions —
//    interleaved output across threads has no ordering contract to check.
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_calls_do_not_crash() {
    std::ostringstream sink;
    std::streambuf* old = std::cout.rdbuf(sink.rdbuf());

    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < 2000; ++i)
                AstraLib::Debug::debugMessage("thread ", t, " iter ", i);
        });
    for (auto& th : ts) th.join();

    std::cout.rdbuf(old);
    // Reaching this line at all is the assertion — a crash fails the whole test binary.
}

int main() {
    std::cout << "debugFunc test suite\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"output_contains_arguments_and_markers",       test_output_contains_arguments_and_markers},
        {"variadic_edge_counts",                         test_variadic_edge_counts},
        {"concurrent_calls_do_not_crash",                test_concurrent_calls_do_not_crash},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        long before = g_failures;
        t.fn();
        std::cout << (g_failures == before ? "[  OK  ] " : "[ BAD  ] ") << t.name << std::endl;
    }

    if (g_failures == 0) { std::cout << "\nALL DEBUGFUNC TESTS PASSED" << std::endl; return 0; }
    std::cerr << "\n" << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
