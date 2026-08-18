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
| `ThreadPoolTest.cpp` | Exactly-once task execution, multi-worker distribution, repeated bursts (300x), concurrent producers, destructor regression guard, dispatcher idle-CPU behavior |
| `TimerTest.cpp` | Output parsing, monotonicity vs. busy-wait duration, cycles/ns self-consistency |
| `TimeNowTest.cpp` | Sane calendar range, agreement with `system_clock`, NS/MS consistency, non-decreasing, concurrent calls |
| `DebugFuncTest.cpp` | Output content/markers, variadic edge counts, concurrent-call smoke test, concurrent padding-correctness regression |
| `AtomicRingBufferTest.cpp` | Basic functional coverage: `isEmpty()` (incl. during an in-flight, unpublished enqueue), `enqueue(const A&)` copy semantics, `emplaceEnqueue`'s trivially-destructible branch, non-default `BATCH_SIZE`, `batchDequeue` count-clamping regression |
| `AtomicRingBufferStressTest.cpp` | Adversarial MPMC suite: exactly-once accounting, torn-write detection, lifetime canaries, oversubscription, `batchDequeue`. Has its own watchdog thread that turns queue deadlocks into diagnosed failures. |

## Known findings

Bugs found by these tests so far, in order of severity:

1. **Fixed** — `AtomicRingBuffer<T, 1>` violated the ring buffer's seq
   invariant (items overwritten unread, or seq going backwards and
   deadlocking). Fixed with `static_assert(SIZE >= 2)`.
2. **Fixed** — `ThreadPool`'s destructor used to never return once the pool
   had processed at least one task (`taskDispatcher`'s inner loop never
   re-checked `running`). Fixed by bounding the inner loop with
   `running && !taskQueue.isEmpty()` (plus a new `AtomicRingBuffer::isEmpty()`).
   Verified: 9 consecutive clean runs. `ThreadPoolTest.cpp`'s
   `destructor_returns_after_use` is kept as the regression guard.
3. **Fixed** — `ThreadPool` could silently lose tasks under load. Root cause:
   `Worker::thread_loop()` reset `poolFlag` (making the worker eligible for
   reassignment) *before* calling `gate.reset()`; a reassignment landing in
   that gap had its wake-up clobbered by the worker's own delayed reset, so
   the new task sat unexecuted and that worker was permanently lost from the
   pool's capacity. Fixed by swapping the order: `gate.reset()` now runs
   *before* `poolFlag.store(0, ...)`, so a worker is never marked reassignable
   until its own gate has already been cleared — closing the window rather
   than narrowing it. Traced through for a residual lost-wakeup risk and
   found none: `ThreadGate::waiter()` always re-checks its flag fresh
   against a literal `0` immediately before each `FUTEX_WAIT`, so a
   `signaler()` landing anywhere in the new ordering is still never missed.
   Verified empirically, not just by source review: 35 total clean runs of
   `ThreadPoolTest` across two independent verification passes (10 + 25),
   each run putting the pool through 300 burst cycles and 16,000
   concurrently-produced tasks — a big jump from the handful of bursts it
   used to take to fail. Full suite also reconfirmed 10/10 clean after this
   change. Given this bug's history of going quiet before resurfacing,
   confidence here rests on the closed race window (verified by trace), not
   just on absence of failures.
4. **Fixed** — `debugMessage()` called `std::localtime`, which is not
   thread-safe (shared static buffer). Was confirmed as a data race under
   ThreadSanitizer, AND segfaulted a plain (non-sanitized) build in 2 of 2
   full `ctest` runs. Fixed by switching to `localtime_r`. Verified clean:
   0/30 repeated runs crashed post-fix, and TSan no longer reports the
   `tzset_internal`/`localtime` race. (Along the way, also fixed a separate,
   test-only bug this had been masking: `DebugFuncTest.cpp`'s concurrency
   test redirected `std::cout` into a single shared `std::ostringstream`
   read/written by 8 threads at once — itself a data race, unrelated to the
   library, that was corrupting the heap almost every run once the real bug
   stopped contributing noise. Fixed by redirecting at the OS file-descriptor
   level instead.)
5. **Fixed** — `debugMessage()`'s `std::setw(6) << std::setfill('0')` used
   to mutate `std::cout`'s shared `ios_base` formatting state (width/fill)
   unsynchronized across threads — confirmed as two TSan data races, and
   later confirmed to actually corrupt real output (28 of 1624 lines
   malformed) via `DebugFuncTest.cpp`'s `concurrent_padding_stays_correct`.
   Fixed by wrapping the whole body of `debugMessage()` in a CAS-based
   spinlock (a file-scope `std::atomic<bool> writing`), serializing all
   calls process-wide. Verified: `concurrent_padding_stays_correct` now
   passes (0 malformed lines).
   That fix briefly introduced its own regression — `writing` was declared
   as a plain namespace-scope global, not `inline`, which is an ODR
   violation in a header-only library (confirmed via a 2-TU repro: linking
   two `.cpp` files that both include the header failed with `multiple
   definition of 'AstraLib::Debug::writing'`). Now fixed with `inline
   std::atomic<bool> writing{false};`. Verified: the same 2-TU repro now
   links and runs cleanly, and the full suite is 10/10. Separately,
   serializing the *entire* function body (not just the `std::cout` usage)
   is broader than strictly needed — `localtime_r` and `system_clock::now()`
   don't need the lock — but that's a performance note, not a correctness
   problem.
6. **Fixed** — `ThreadPool`'s dispatcher used to busy-spin instead of
   sleeping once it had processed its first task, permanently pinning a
   full CPU core for the rest of that pool's life (`taskGate`'s flag was
   set once and never reset anywhere in `ThreadPool` itself). Fixed by
   adding `if (taskQueue.isEmpty()) { taskGate.reset(); }` after the
   dispatcher's inner loop. Traced through for a lost-wakeup risk (the
   classic failure mode for this kind of check-then-reset pattern) and
   found safe: `ThreadGate::waiter()` always re-checks the flag fresh
   against a literal `0` immediately before each `FUTEX_WAIT` attempt,
   so a `signaler()` landing in the gap is never missed. Verified:
   `dispatcher_idle_behavior`'s CPU consumption dropped from ~10s to
   0.000037s over the same 2s idle window.
7. **Fixed** — `AtomicRingBuffer::batchDequeue(count)` had no bound check
   against `count > BATCH_SIZE`; `batchDequeueArray` is a fixed
   `std::array<A, BATCH_SIZE>` sitting immediately before `buffer[SIZE]` in
   the class layout, so an oversized request wrote straight past it into
   the ring buffer's own data slots. No concurrency needed to trigger it —
   confirmed with a single-threaded repro under UBSan:
   `index 3 out of bounds for type 'int [2]'`. Fix went through two broken
   attempts first: a `static_assert(count > BATCH_SIZE, ...)` (illegal —
   `count` is a runtime parameter, not a compile-time constant) and then a
   runtime clamp against a `const int count` parameter (illegal — can't
   reassign a `const` parameter). Final fix: dropped `const` from the
   parameter, kept the runtime clamp (`if (count > BATCH_SIZE) count =
   BATCH_SIZE;`). Verified clean under UBSan, including confirming the
   clamped call only actually dequeues `BATCH_SIZE` items and leaves the
   rest in the queue in correct FIFO order. Full suite 10/10 after.
   Regression test added: `AtomicRingBufferTest.cpp`'s
   `batch_dequeue_clamps_to_batch_size` checks the logical contract (clamped
   count, correct FIFO remainder) in every build; running the suite under
   `-DENABLE_ASAN=ON` additionally pulls in UBSan's hard bounds check on
   top. Verified both ways: clean under a plain build and under ASan/UBSan.

These are left for deliberate fixes rather than patched inside the test
files — the tests exist to locate and explain them precisely, not to paper
over them.

## API note

`AtomicFutex` was simplified from a template (`AtomicFutex<T>` with a
`customValue`/`setCustomValueForWait`/`setCustomValueForWake` mechanism) down
to a plain `wait()`/`wake()` type. `AtomicRingBuffer`'s three futex-based
dequeue methods (`futexDequeueWake()`, `futexDequeueWait()`, and finally
`futexDequeueWaitWithCountTimer()`) have all since been removed — none was
called anywhere in the codebase, and the last one's own spin-then-sleep bug
(see below) is moot now that the method is gone along with it.
`AtomicRingBuffer` has no futex-aware method left; `dequeue()`/`enqueue()`
are pure spin. Separately, `enqueueptr()` and `noMoveEnqueue()` (which had
become byte-for-byte identical to each other) were replaced by a proper
`enqueue(const A&)` overload alongside the existing `enqueue(A&&)` — the
standard move/copy overload pair, e.g. `std::vector::push_back`'s.
`AtomicFutex` itself is untouched by any of this and is still exercised
directly by `AtomicFutexTest.cpp`.

**Not a bug, decided explicitly:** `AtomicFutex` is a bare, edge-triggered
futex wrapper — `wait()` has no memory of a `wake()` that happened before it
was called (it snapshots the futex value at call time, so a stale post-wake
value looks unchanged to it and it blocks forever). That's the same
discipline every real futex-based primitive requires: the caller must check
their own condition immediately before calling `wait()`, the way `ThreadGate`
in `ThreadPool.hpp` does. `wait()`'s doc comment ("Will wait unless there has
been a wake call") describes a different, sticky/semaphore-like contract
this implementation doesn't provide and was never meant to — that comment is
stale/wrong, not the behavior. No test asserts on this by design; see
`AtomicFutexTest.cpp`'s file header for the reasoning.
