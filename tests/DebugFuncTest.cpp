// ─────────────────────────────────────────────────────────────────────────────
// Tests for AstraLib::Debug::debugMessage.
//
// debugMessage only prints (void return, no observable state besides
// stdout), so the single-threaded tests below redirect std::cout's streambuf
// into a std::ostringstream for the duration of a call and assert on the
// captured text — same technique used in TimerTest.cpp. The concurrent test
// uses OS-level fd redirection instead; see its own comment for why.
//
// FIXED BUG (kept documented here so the regression stays caught): debugMessage
// used to call std::localtime(), which per glibc's own docs is NOT thread-safe
// — it returns a pointer into a single shared static buffer. Two threads
// logging "simultaneously" (exactly the use case a function that prints its
// own TID exists for) raced on that shared buffer. Confirmed both as a TSan
// data race (pointing at glibc's tzset_internal/localtime) and as an actual
// segfault in 2 of 2 full `ctest` suite runs — a real crash risk without a
// sanitizer too, not just a theoretical TSan-only finding. Fixed by switching
// to localtime_r (reentrant, writes into a caller-supplied std::tm instead of
// a shared static one) in the header. test_concurrent_calls_do_not_crash below
// is what will catch a regression: reaching its end at all is the assertion.
// Verified: 0/30 repeated runs crashed post-fix (was crashing on essentially
// every run once the test's own separate bug — see below — was also fixed).
//
// STILL OPEN, found while verifying the fix above: debugMessage's
// `std::setw(6) << std::setfill('0')` (debugFunc.hpp) mutates std::cout's
// shared ios_base formatting state (width/fill), which is NOT synchronized
// across threads — confirmed as two TSan data races (ios_base::width and
// ::fill) when called concurrently. This is separate from the localtime bug:
// it doesn't crash (30/30 plain-build runs were clean), but it's still
// undefined behavior per the C++ memory model, and in principle could produce
// wrong padding (one thread's setw/setfill consumed by another's output)
// even though that hasn't been observed. Standard fix: build the formatted
// line into a local std::ostringstream first, then do one unformatted
// `std::cout << line;` — stateful manipulators only touch the local stream.
// test_concurrent_padding_stays_correct below checks for the observable
// symptom directly (malformed padding), but since the race is intermittent
// it may well pass even with the bug still present — it's a tripwire, not
// proof of absence.
// ─────────────────────────────────────────────────────────────────────────────

#include <AstraLib/Debug/debugFunc.hpp>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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
//    not crash the process — this is the regression test for the now-fixed
//    localtime race (see file header). No content assertions — interleaved
//    output across threads has no ordering contract to check.
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_calls_do_not_crash() {
    // Redirect at the OS file-descriptor level (dup2), not by swapping cout's
    // rdbuf to a user-level std::ostringstream: 8 threads writing concurrently
    // into a shared std::ostringstream's internal buffer is itself a data race
    // (ostringstream isn't synchronized) and previously caused real heap
    // corruption here, unrelated to and separate from debugMessage's own bug.
    // The real stdout FILE* (which cout funnels through, sync_with_stdio being
    // on by default) is safe for concurrent writes per the standard.
    std::fflush(stdout);
    int savedStdout = dup(STDOUT_FILENO);
    int devNull = open("/dev/null", O_WRONLY);
    dup2(devNull, STDOUT_FILENO);
    close(devNull);

    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < 2000; ++i)
                AstraLib::Debug::debugMessage("thread ", t, " iter ", i);
        });
    for (auto& th : ts) th.join();

    std::fflush(stdout);
    dup2(savedStdout, STDOUT_FILENO);
    close(savedStdout);
    // Reaching this line at all is the assertion — a crash fails the whole test binary.
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Regression test for the STILL-OPEN setw/setfill race documented in the
//    file header: under concurrent debugMessage calls, one thread's
//    std::setw(6)/std::setfill('0') could in principle be consumed by
//    another thread's output (they share std::cout's ios_base state), which
//    would show up as a microseconds field that isn't exactly 6 digits.
//    Redirects to a real temp file (not /dev/null, like test 3) so the
//    content can be read back and checked. NOTE: the race is confirmed by
//    TSan but has never been observed to actually corrupt output in a plain
//    build, so this test may well pass even though the underlying race is
//    still there — it's the tripwire for if/when it ever does misfire, not
//    proof the race is gone. Fixing the race for real means building each
//    line into a local std::ostringstream in debugMessage() rather than
//    applying stateful manipulators straight to the shared std::cout.
// ─────────────────────────────────────────────────────────────────────────────
static void test_concurrent_padding_stays_correct() {
    std::fflush(stdout);
    char tmpPath[] = "/tmp/debugfunc_pad_test_XXXXXX";
    int tmpFd = mkstemp(tmpPath);
    CHECK_CTX(tmpFd != -1, "mkstemp failed, cannot run this test");
    if (tmpFd == -1) return;

    int savedStdout = dup(STDOUT_FILENO);
    dup2(tmpFd, STDOUT_FILENO);
    close(tmpFd);

    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < 500; ++i)
                AstraLib::Debug::debugMessage("t", t, " i=", i);
        });
    for (auto& th : ts) th.join();

    std::fflush(stdout);
    dup2(savedStdout, STDOUT_FILENO);
    close(savedStdout);

    std::ifstream in(tmpPath);
    std::string line;
    std::regex re(R"(\[\d{2}:\d{2}:\d{2}\.(\d+)\])");
    long checked = 0, malformed = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            ++checked;
            if (m[1].length() != 6) ++malformed;
        }
    }
    in.close();
    std::remove(tmpPath);

    CHECK_CTX(checked > 0, "no timestamped lines found in captured output — test setup problem");
    CHECK_CTX(malformed == 0, malformed << "/" << checked << " line(s) had a microseconds field "
              "that wasn't exactly 6 digits — the setw/setfill race in the file header just "
              "produced observably wrong output, not just a TSan report");
}

int main() {
    std::cout << "debugFunc test suite\n" << std::endl;

    struct { const char* name; void (*fn)(); } tests[] = {
        {"output_contains_arguments_and_markers",       test_output_contains_arguments_and_markers},
        {"variadic_edge_counts",                         test_variadic_edge_counts},
        {"concurrent_calls_do_not_crash",                test_concurrent_calls_do_not_crash},
        {"concurrent_padding_stays_correct",             test_concurrent_padding_stays_correct},
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
