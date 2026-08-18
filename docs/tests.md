# Testing

Tests live in `tests/`, one file per header, built with CMake and run through
CTest. Every test uses real assertions (a `CHECK`-style macro) that report
pass/fail with a diagnostic — not print-and-eyeball output.

## Build and run

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Every test has a hard ctest-level `TIMEOUT` (120s, 900s for the stress suite)
so a hang gets SIGKILLed and reported instead of hanging CI — several of the
bugs below were found precisely because a test *didn't* return.

### Sanitizer builds

```
cmake -S . -B build-tsan -DENABLE_TSAN=ON   # data races
cmake -S . -B build-asan -DENABLE_ASAN=ON   # memory errors + UB
```

Run stress-style tests with `ASTRA_STRESS_SCALE=10` (or higher) under a
sanitizer to keep runtime reasonable — sanitizer instrumentation is slow.

Note: on this machine (gcc-11, WSL2/newer kernel), TSan's runtime can abort
with `unexpected memory mapping` due to ASLR entropy. Work around it with
`setarch $(uname -m) -R ./your_test`.

## What's covered

| File | Covers |
|---|---|
| `SpinlockTest.cpp` | Mutual exclusion under contention (raw lock/unlock, RAII guard, custom backoff, wide critical sections) |
| `PaddedAtomicTest.cpp` | Cache-line layout/alignment, array stride, no cross-talk between adjacent elements under concurrent writes |
| `AtomicFutexTest.cpp` | wait/wake handoff, repeated turn-taking cycles, wake-releases-one-waiter semantics |
| `ThreadSafeIndexPoolTest.cpp` | Initial fill uniqueness, no double-checkout under contention/exhaustion, `reInitializePool()` |
| `ThreadPoolTest.cpp` | Exactly-once task execution, multi-worker distribution, repeated bursts, concurrent producers |
| `TimerTest.cpp` | Output parsing, monotonicity vs. busy-wait duration, cycles/ns self-consistency |
| `TimeNowTest.cpp` | Sane calendar range, agreement with `system_clock`, NS/MS consistency, non-decreasing, concurrent calls |
| `DebugFuncTest.cpp` | Output content/markers, variadic edge counts, concurrent-call smoke test |
| `AtomicRingBufferStressTest.cpp` | Adversarial MPMC suite: exactly-once accounting, torn-write detection, lifetime canaries, oversubscription, `batchDequeue`. Has its own watchdog thread that turns queue deadlocks into diagnosed failures. |

## Known findings

Bugs found by these tests so far, in order of severity:

1. **Fixed** — `AtomicRingBuffer<T, 1>` violated the ring buffer's seq
   invariant (items overwritten unread, or seq going backwards and
   deadlocking). Fixed with `static_assert(SIZE >= 2)`.
2. **Open** — `ThreadPool`'s destructor never returns once the pool has
   processed at least one task (`taskDispatcher`'s inner loop never
   re-checks `running`). Confirmed 100% reproducible — any code that lets a
   used `ThreadPool` clean up normally (RAII, return, unwinding) hangs
   right there. See `ThreadPoolTest.cpp`'s `destructor_returns_after_use`
   test and its file-header comment for the full mechanism.
3. **Open** — `ThreadPool` can silently lose tasks under load. Confirmed
   repeatedly — fires in most full `ctest` runs of `ThreadPoolTest.cpp`
   (2-3 of its 4 task-volume sub-tests per run) and in 1 of 3 isolated
   30,000-task probes, though not deterministically every time. Likely
   mechanism: `Worker::thread_loop()` resets `poolFlag` (making the
   worker eligible for reassignment) *before* calling `gate.reset()`; a
   reassignment landing in that gap has its wake-up clobbered by the
   worker's own delayed reset, so the new task sits unexecuted and that
   worker is permanently lost from the pool's capacity. See
   `ThreadPoolTest.cpp`'s file header (offered as the likely explanation
   from source review, not confirmed by forcing the exact interleaving).
   Worst case, observed: if all 4 workers are eventually lost this way, the
   dispatcher deadlocks entirely and even `assignTask()` starts spinning
   forever once the internal task queue fills up — not just task
   completion. This has caused `ThreadPoolTest` to hit ctest's TIMEOUT with
   no in-test diagnostic at all, since it got stuck before its own bounded
   wait was ever reached.
4. **Open** — `AtomicFutex::wait()` blocks forever if `wake()` was already
   called before it, contradicting its own doc comment ("Will wait unless
   there has been a wake call"). See `AtomicFutexTest.cpp`'s
   `wake_before_wait_should_not_block` test.
5. **Open** — `debugMessage()` calls `std::localtime`, which is not
   thread-safe (shared static buffer). Confirmed as a data race under
   ThreadSanitizer, AND segfaulted a plain (non-sanitized) build in 2 of 2
   full `ctest` runs — a real crash risk without a sanitizer too, despite
   not reproducing in smaller standalone probes during development. Fix is
   `localtime_r`. See `DebugFuncTest.cpp`'s file header.

These are left for deliberate fixes rather than patched inside the test
files — the tests exist to locate and explain them precisely, not to paper
over them.

## API note

`AtomicFutex` was simplified from a template (`AtomicFutex<T>` with a
`customValue`/`setCustomValueForWait`/`setCustomValueForWake` mechanism) down
to a plain `wait()`/`wake()` type. `AtomicRingBuffer`'s three futex-based
dequeue methods (`futexDequeueWake()`, `futexDequeueWait()`, and finally
`futexDequeueWaitWithCountTimer()`) have all since been removed — none was
called anywhere in the codebase, and the last one's own bug (the same
wait()/wake() ordering issue as finding 4 below, reachable via its spin-then-
sleep path) is moot now that the method is gone along with it.
`AtomicRingBuffer` has no futex-aware method left; `dequeue()`/`enqueue()`
are pure spin. Separately, `enqueueptr()` and `noMoveEnqueue()` (which had
become byte-for-byte identical to each other) were replaced by a proper
`enqueue(const A&)` overload alongside the existing `enqueue(A&&)` — the
standard move/copy overload pair, e.g. `std::vector::push_back`'s.
`AtomicFutex` itself is untouched by any of this and is still exercised
directly by `AtomicFutexTest.cpp`, including finding 4 below.
