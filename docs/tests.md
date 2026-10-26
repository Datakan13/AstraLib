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

Every test has a hard ctest-level `TIMEOUT` (120s by default; 240s for
`ThreadPoolTest`, 300s for `SpscRingBufferTest`, 900s for
`AtomicRingBufferStressTest` and the perf benchmark, 1800s for the comparison
benchmarks) so a hang gets SIGKILLed and reported instead of hanging CI — several of the
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

### Automatic scaling on small machines

Several tests deliberately spawn more threads than a typical dev machine has
cores, with `std::this_thread::yield()` (or, for `AtomicRingBufferStressTest`,
tight busy-spin contention) *inside* the held lock/slot — a technique used
throughout this suite to widen the timing window a broken lock/pool could
slip through. That's harmless on a many-core machine, but on a small CI
runner (GitHub's hosted `ubuntu-latest` gives 2 vCPUs) it's catastrophic, not
just slower: confirmed directly via `taskset -c 0,1` locally, e.g.
`SpinlockTest`'s `mutual_exclusion_wide_critical_section` at a true 1:1
thread:core match took 1.7ms; at just 2x oversubscription on the same
constrained run, it took 17 *seconds* — roughly a 10,000x cliff, not a
proportional slowdown. The same pattern reproduced in
`ThreadSafeIndexPoolTest` (0.3ms → 8.1s) and `AtomicRingBufferStressTest`'s
`mpmc_torn_writes` (1.2ms at 1:1 → still not finished after 45s at 2x).

Fix: `SpinlockTest.cpp`'s `mutual_exclusion_wide_critical_section`,
`ThreadSafeIndexPoolTest.cpp`'s `no_double_checkout_under_contention`, and
every P/C-thread test in `AtomicRingBufferStressTest.cpp` now cap their
thread counts to `std::thread::hardware_concurrency()` automatically at
runtime — no env var, no manual configuration. On any machine with 8+ cores
(including the one this suite was developed and verified on all session)
they're unchanged from their original, already-validated parameters; below
that, thread counts scale down to a true 1:1 match with available cores.
`test_oversubscription`'s whole purpose is deliberate oversubscription, so
it keeps that character on capable machines but caps down the same way on
tiny ones — a 2-core runner was never going to meaningfully exercise "8x
oversubscription" reliably regardless.

`std::thread::hardware_concurrency()` reads `/sys/devices/system/cpu/online`
directly (confirmed via `strace`), not `sysconf()`, so the branch itself
couldn't be faked locally with `taskset` or `LD_PRELOAD` (both tried) — only
the chosen parameter values were verified locally, not the branch firing.
Confirmed on GitHub Actions' real 2-vCPU runner: full suite green,
`SpinlockTest`/`ThreadSafeIndexPoolTest`/`AtomicRingBufferStressTest` all
fast (0.11s/0.01s/0.06s) rather than hitting their old deadlines — the
`hw < 8` branch does fire correctly there.

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
| `SpscRingBufferTest.cpp` | `spscRingBuffer`: single-threaded FIFO across wraparound, exact capacity (full ring blocks the producer without overwriting; empty ring blocks the consumer), SIZE 1/2/3 lockstep, 5M-item strict-FIFO stream, torn-payload detection at 120B and 256B, lifetime canaries through both `enqueue` and `emplaceEnqueue`, `unique_ptr` ownership, ping-pong. Own watchdog. Verified by planting the bugs from earlier drafts (`==` for `=`, relaxed publish) and three others; every one is caught (relaxed ordering only under TSan). |
| `AtomicRingBufferPerformanceTest.cpp` | Benchmark, not pass/fail (label `perf`, excluded from the default run): SPSC ceiling, MPMC scaling, handoff latency percentiles, `batchDequeue` vs `dequeue`, payload size. See "Benchmarking" below. |

## Benchmarking

`AtomicRingBufferPerformanceTest` is a benchmark, not a test. It reports a
table and asserts only that the work really happened (exact item accounting,
plus a structural-failure floor of 100k ops/s). It carries the ctest label
`perf` and is excluded from the default run, so `ctest` and CI use
`-LE perf`. Run it deliberately:

```
ctest --test-dir build -L perf --output-on-failure
ASTRA_PERF_VERBOSE=1 ./build/tests/AtomicRingBufferPerformanceTest  # per-rep numbers
```

Absolute throughput is deliberately **not** gated. The hosted runner's
variance would produce far more false failures than regressions caught, and
some configurations are bimodal even on a quiet 20-core dev machine (see
below). The test also needs at least 2 *physical* cores and prints SKIPPED
below that — a 2-vCPU runner is typically one physical core plus SMT.

Methodology (the parts that turned out to matter, each learned the hard way
while writing it):

* **Pin one thread per PHYSICAL core, not per hardware thread — but know
  what that buys you here.** cpu0 and cpu1 are SMT siblings of one core
  (`topology/thread_siblings_list` reports `0-1` for both), so the obvious
  pinning put a producer and consumer spinning on `_mm_pause` against each
  other inside a single core; fixing that removed a large error. But on this
  host the topology sysfs reports is **synthetic** and the guest vCPU -> host
  core mapping is not fixed — so pinning removes migration and sibling
  collisions, but does not pin you to a known piece of silicon. See finding 9
  for what that does and does not explain.
* **Don't compare `__rdtsc()` across cores.** Measuring one-way latency by
  stamping in the producer and subtracting in the consumer reported a max of
  6.8e18 ns — an unsigned wrap of a *negative* interval. Per-core TSCs are
  synchronized only to within an offset even with `constant_tsc`/
  `nonstop_tsc`, and `rdtsc` doesn't serialize. Discarding the negative
  samples would have been worse than the visible break: the skew biases every
  sample, so the survivors would be quietly wrong. Replaced with a ping-pong
  round trip where one pinned thread takes both timestamps.
* **Report the range, not just the median.** See finding 9.
* Timer is used only for aggregate runs, where its ~24k-44k-cycle `__cpuid`
  overhead (see `TimerTest.cpp`'s header) amortizes away, and it is
  cross-checked against `steady_clock` at startup — if its GHz calibration
  were off, every Timer-derived figure would be scaled by the same factor
  silently. Measured ratio: 0.9995.

Representative numbers, 20-core WSL2 dev machine, `-O3`, median of 15 runs:

| Measurement | Result |
|---|---|
| SPSC 1P/1C | ~27-130 M ops/s (bimodal — environment, see finding 9) |
| MPMC 2P/2C | ~33 M ops/s (~29 ns/op) |
| MPMC 4P/4C | ~30 M ops/s (~33 ns/op) |
| Handoff latency | p50 139 ns round trip / ~70 ns one-way; p99.9 ~234 ns |
| `batchDequeue(50)` vs `dequeue()` | 0.85-1.03x across measurements — no gain, see finding 10 |
| Payload 4B / 56B / 120B | ~95 / ~49 / ~23 M ops/s |

### Comparison benchmarks (`bench/`)

Head-to-head runs of AstraLib against established libraries, one binary per
component. They live outside `tests/` behind a CMake option that is **OFF by
default**, because turning it on downloads the competitors (header-only,
pinned by SHA256 in `bench/CMakeLists.txt`) and needs Boost headers:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_COMPARISON_BENCH=ON
cmake --build build -j
./build/bench/QueueComparisonBench        # also Spinlock-, ThreadPool-, IndexPool-
ctest --test-dir build -L comparison -V   # or all four through ctest
```

They're labelled `perf;comparison`, so `ctest -LE perf` still skips them.
Knobs: `ASTRA_BENCH_SCALE`, `ASTRA_BENCH_MULT`, `ASTRA_BENCH_REPS` (default 7),
`ASTRA_BENCH_VERBOSE`.

| Binary | AstraLib component | Competitors |
|---|---|---|
| `QueueComparisonBench` | `AtomicRingBuffer`, `spscRingBuffer` (1P/1C only) | rigtorp::MPMCQueue, boost::lockfree::queue, moodycamel::ConcurrentQueue, mutex+deque; SPSC-only in 1P/1C: rigtorp::SPSCQueue, moodycamel::ReaderWriterQueue, boost::lockfree::spsc_queue |
| `SpinlockComparisonBench` | `Spinlock` (default and `waitTime=16`) | TTAS spinlock, `pthread_spin_lock`, `std::mutex` |
| `ThreadPoolComparisonBench` | `ThreadPool<W>` | BS::thread_pool v5, textbook mutex+condvar pool |
| `IndexPoolComparisonBench` | `ThreadSafeIndexPool` | rigtorp::MPMCQueue, boost::lockfree::stack, moodycamel::ConcurrentQueue, mutex+vector |

Same methodology as `AtomicRingBufferPerformanceTest`: one thread per
physical core, configurations that don't fit are skipped, median + range
of the reps. One addition: competitors are **interleaved per rep**
(A B C A B C …) so sustained-load frequency decay doesn't favour whichever
library runs first. Each table has a "vs Astra" column; read that rather than
comparing absolute numbers across separate runs. Each benchmark also checks
its own accounting: queue value sums, a plain counter inside the lock, and
per-index plain counters for the pools. An adapter that loses or duplicates
work fails the run instead of posting a flattering number.

Headline results, 2026-10-26, same dev machine, `-O3`, median of 7:

| Comparison | Result |
|---|---|
| Queue, MPMC shapes | AtomicRingBuffer 1.07-1.16x rigtorp::MPMCQueue (same design) at 2P/2C, 4P/4C, 4P/1C, 1P/4C; ~2x moodycamel; 7-10x boost::lockfree::queue |
| Queue, 4P/1C | moodycamel::ConcurrentQueue 1.17x faster (per-producer sub-queues suit MPSC; it's unbounded and not FIFO) |
| Queue, 1P/1C | rigtorp::SPSCQueue and moodycamel::ReaderWriterQueue ~2x faster, as expected — they skip the shared ticket RMW; boost::lockfree::spsc_queue on par |
| `spscRingBuffer`, 1P/1C | 191 M ops/s median (range 179-203) — highest median of the four SPSC queues and the tightest range; moodycamel::ReaderWriterQueue 180 (132-279), rigtorp::SPSCQueue 128 (84-228), boost::lockfree::spsc_queue 103 (62-151). Ping-pong p50 126 ns, lowest of all nine queues |
| Queue latency | best p50 (131 ns) and p99.9 (228 ns) round trip of all eight |
| Spinlock throughput | highest at every contended point (2.5-5.3x TTAS/pthread at 8 threads)… |
| Spinlock **handoffs** | …but it hands the lock to another thread 10-1000x less often (0.01-0.28 M/s vs 0.6-14 M/s). The 1000-pause backoff lets the holder re-acquire over and over, so waiters can sit for ~100 µs. `waitTime=16` gives up to ~55% of the throughput in exchange for much more frequent handoffs |
| ThreadPool burst | flat ~0.11-0.21 M tasks/s regardless of worker count or task size, i.e. ~5-9 µs per task (one unstable 2-worker row reached 0.36). With ~1 µs tasks at 2-4 workers, BS::thread_pool is 7-17x faster and the condvar pool 8-10x |
| ThreadPool wake latency | p50 ~27 µs vs ~13-14 µs for both competitors (idle pool → task start) |
| IndexPool | 1.02-1.14x rigtorp::MPMCQueue at 2-8 threads (same ring underneath), 1.7x it uncontended; 2.6-8x boost::lockfree::stack under contention |

Caveats specific to these numbers:
* Thread-pool workers are created inside each library and can't be pinned, so
  that table is noisier than the others. Both competitors also slowed sharply
  at 8 workers on this box (BS::thread_pool: ~1.5-2 → 0.05 M tasks/s). That
  hasn't been investigated, so don't read the 8-worker rows as a ranking.
* The TTAS/pthread near-zero fairness (0.02-0.10) at 8 threads with the long
  critical section is real starvation: some threads got almost no
  acquisitions in 200 ms. In an earlier run one thread got none at all.

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
8. **Fixed, test-suite-only — not a library bug.** Three tests
   (`SpinlockTest`, `ThreadSafeIndexPoolTest`, `AtomicRingBufferStressTest`)
   hung/timed out on GitHub Actions' 2-vCPU runner despite being fully clean
   on this machine's 20 cores. Root cause was in the tests, not the library:
   deliberate thread-oversubscription-plus-yield-while-holding patterns that
   are fine with headroom but catastrophic (~10,000x, not proportional) once
   threads outnumber real cores. See "Automatic scaling on small machines"
   above for the fix and its verification limits.

9. **Not a bug. Cause UNIDENTIFIED — read this entry as a list of what has
   been ruled out.** With 1-2 threads, throughput is bimodal by ~5x
   (identical 1P/1C reps: 26.8, 39.3, 135.2, 98.2, 113.8, 119.4, 132.2,
   47.7, 58.1 M ops/s). Two successive explanations were written into this
   file as established and **both were wrong**; they are recorded here so the
   same ground is not re-covered:

   * ~~Ring occupancy~~ (`ticket & (SIZE-1)` making write index == read index
     when full/empty). Never tested before being asserted. Still untested —
     isolating it needs occupancy varied while placement is held constant.
   * ~~Heterogeneous P/E cores behind WSL2's synthetic topology.~~ Plausible
     (the part really is 6 P + 8 E, and sysfs really does report a fake
     uniform 10x2), and supported by two pinned pair sweeps showing a stable
     6-8x spread. **It did not survive a controlled test.** Both sweeps
     measured pairs in ascending order, so "early" and "low cpu id" were
     confounded. Running the sweep in reverse:

     ```
     forward:  cpu0+2 42.6   cpu4+6 86.5   cpu8+10 107.4  cpu16+18 104.4
     reverse:  cpu0+2 96.0   cpu4+6 50.7   cpu8+10 122.1  cpu16+18  85.2
     ```

     The slow spot moves; it does not track cpu id.

   **What the probes positively establish** (both are built into the
   benchmark and printed every run):

   * Single-thread speed across the ten cores varies by only **1.09x** — they
     are near-uniform on scalar work.
   * Pairwise ping-pong handoff cost varies by only **1.46x** (228-334
     cycles, 85-124 ns).

   Neither can produce a 5x throughput swing, and ordering cores by either
   metric did not reduce the bimodality at all. The benchmark therefore
   measures both, reports them, and **only reorders cores when the pairwise
   spread exceeds 1.5x** — acting on a 1.46x spread whose ranking does not
   reproduce would be fitting noise.

   **Separately verified and real: sustained-load frequency decay.** A
   fixed-work canary on an otherwise idle core (SMT sibling deliberately left
   idle, so not sibling contention) cost 34.9M TSC cycles cold, 56.7M after
   10s of all-core load, 73.6M after 60s, ~9% unrecovered immediately after.
   The benchmark now re-probes at the end and prints the drift (typically
   ~1.1x over a default run). This is why longer runs are not automatically
   better: `ASTRA_PERF_MULT=25` pushes later reps deep into decay.

   **Remaining candidates, untested:** hypervisor vCPU scheduling (Hyper-V's
   root scheduler descheduling a vCPU mid-run), turbo/frequency transitions,
   and the original occupancy hypothesis. Distinguishing them needs a bare-
   metal Linux host, which would remove the largest confound outright.

   **Practical guidance, which does not depend on knowing the cause:** trust
   cross-configuration comparisons *within* one run; treat absolute numbers
   *across* runs as soft. Rows that stay tight (4P/4C, 120-byte payloads) are
   the trustworthy ones — more threads average over whatever this is. This
   was corroborated accidentally: a run contending with unrelated CPU load
   collapsed the 1-2 thread rows ~5x while 4P/4C moved 30.2 -> 30.9 and
   120-byte payloads 23.7 -> 24.4.

10. **Not a bug — `batchDequeue()` doesn't amortize, and a slot-size
   experiment explained a misleading "speedup".** With 64-byte slots it
   measures **0.85-1.03x** the speed of the same number of `dequeue()` calls
   across separate measurements — no real gain either way. Reading the implementation confirms why: its loop body does exactly
   the same per-item work as `dequeue()` — one `readTicket.fetch_add`, one seq
   spin, one move, one seq store — and then additionally stages each item into
   `batchDequeueArray`. It amortizes nothing; it adds a copy. (An earlier
   measurement of 1.66x *faster* was an artifact of the SMT mispinning
   described under "Benchmarking".)

   Slots were briefly padded to 128 bytes (2026-10-26) on the theory that it
   would stop neighbouring slots interfering through Intel's adjacent-line
   prefetcher, and batching then measured 1.05-1.43x *faster*. A same-run A/B
   — three builds of one commit differing only in `alignas(64/128/256)`,
   rotated across rounds on the same pinned physical cores — showed what
   actually happened (ns/item, median across rounds; lower is better):

   | Shape | 64 B | 128 B | 256 B |
   |---|---|---|---|
   | 1P/1C drain, `dequeue()` | **9.0** | 15.2 | 9.9 |
   | 1P/1C drain, `batchDequeue(50)` | 10.6 | 11.5 | 10.1 |
   | Ping-pong round trip (p50 cycles) | 473 | 459 | 478 |
   | 2P/2C | **30.6** | 33.2 | 35.1 |
   | 4P/4C | 34.7 | 35.0 | 34.8 |

   Batching never got faster; plain `dequeue()` got ~60% *slower* at 128
   bytes specifically (reproduced on two different core pairs), and the ratio
   read that as a batching speedup. The prefetcher benefit didn't appear
   either: the near-empty ping-pong handoff, where adjacent-slot interference
   would matter most, was flat across all three sizes. 128 wasn't faster in
   any shape, so slots went back to 64 bytes. Why 128 specifically slows the
   drain (while 64 and 256 don't) is **not** isolated — proving a cause needs
   hardware counters or prefetcher control, neither available under WSL2, and
   this is one hybrid laptop part. Also ruled out along the way: the perf
   test's per-item `doNotOptimize` clobber (spacing it to once per 50 items
   changed nothing).

   Left alone per the convention below; the fix, if wanted, is to claim a run
   of `count` tickets with a single `fetch_add` rather than one per item —
   which is what the `BufferBatchRework` branch is doing.

11. **Not a bug — slot size is a per-queue measurement, not a rule.** The two
   ring buffers settled on *different* slot alignments, both by measurement:
   `AtomicRingBuffer` uses 64 bytes (finding 10's table: 128 was never
   faster) and `spscRingBuffer` uses 128. The same-run A/B for
   `spscRingBuffer` (three builds of one commit differing only in
   `spscSlot`'s `alignas`, 1024-slot ring, 12 rotating rounds on each of two
   pinned core pairs; ns/item, lower is better):

   | Shape | 64 B | 128 B | 256 B |
   |---|---|---|---|
   | 8-byte stream, cpu0 → cpu2 | 5.44 | 4.54 | 4.41 |
   | 8-byte stream, cpu8 → cpu10 | 5.42 | 4.44 | 4.38 |
   | 56-byte stream, cpu0 → cpu2 | 6.56 | 6.14 | 5.57 |
   | 56-byte stream, cpu8 → cpu10 | 6.76 | 5.86 | 5.52 |
   | Ping-pong p50 (cycles), the two pairs | 403 / 420 | 410 / 394 | 355 / 367 |

   128 beats 64 by ~18% on 8-byte items, with no overlap between the two
   sizes' per-round ranges on either core pair. 256 is a little faster again
   (~3% at 8 bytes, ~10% at 56) but its ranges overlap 128's and it doubles
   the ring's memory, so 128 stays. Ping-pong is bimodal (~200-500 cycles at
   every size) and doesn't rank the sizes.

   **Why slot size matters at all.** The claim/publish code runs the same
   instructions at every slot size. What slot size changes is which cache
   lines each core touches, and moving a line between cores (tens of ns) is
   the dominant per-item cost in both designs — far more than the
   instructions. So layout can matter even though the logic doesn't change.

   **Why the two queues could disagree — candidate explanations, NOT
   verified:**
   * Intel's adjacent-line prefetcher fetches aligned 128-byte pairs. With
     64-byte slots, slots *i* and *i+1* share a pair, so the consumer reading
     *i* may also pull *i+1*. That helps if the producer already wrote *i+1*
     (a free early fetch) and hurts if it is still writing it (the line
     bounces back). With 128-byte slots the interaction disappears both
     ways. Which case dominates depends on how far ahead the producer runs,
     and that emerges from each design's per-side cost (shared ticket
     `fetch_add` vs a private cursor) — so the same slot size can land the
     two queues in opposite regimes.
   * That doesn't explain everything: it predicts 128 ≈ 256, which holds for
     `spscRingBuffer` but not `AtomicRingBuffer` (128 was ~60% slower on the
     drain, 64 and 256 fine). Something specific to 128 is involved there.
     One candidate: with 128-byte slots, slots 32 apart are exactly 4 KB
     apart, which can trigger the CPU's 4K-aliasing false dependency between
     loads and stores.

   Experiments that would decide it: sample ring occupancy
   (`writeTicket - readTicket`, or the SPSC cursor gap) during the drain at
   each slot size — the prefetch explanation predicts the two queues run at
   different fill levels; and change the ring size or base alignment to move
   the 4 KB pattern. Hardware counters or prefetcher control would settle it
   directly but aren't available under WSL2.

   **Practical rule:** don't change either alignment to "match" the other,
   and don't change either without rerunning this A/B — the layout
   `static_assert`s in the stress and SPSC tests pin each choice. Results
   are from one hybrid laptop CPU; a different microarchitecture may differ.

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
is moot now that the method is gone along with it.
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
in `ThreadPool.hpp` does. `wait()` used to carry a doc comment ("Will wait
unless there has been a wake call") describing a different, sticky/
semaphore-like contract this implementation doesn't provide and was never
meant to; that comment has since been removed. No test asserts on this by
design; see `AtomicFutexTest.cpp`'s file header for the reasoning.
