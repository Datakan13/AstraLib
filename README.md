# AstraLib
![CI](https://github.com/Datakan13/AstraLib/actions/workflows/ci.yml/badge.svg)

AstraLib grew out of building low-latency trading infrastructure, where determinism and shaving off every possible nanosecond mattered more than being able to handle every workload well. General-purpose concurrency primitives — `std::mutex`, off-the-shelf concurrent queues — are built to be correct and reasonable across a huge range of use cases, and that generality has a cost. AstraLib takes the opposite bet: a small set of primitives, each built for one narrow pattern, so you only pay for the guarantees you actually need.

It's a C++20 concurrency library: atomic helpers, an MPMC ring buffer, a thread-safe debug function, a thread-safe index pool, a thread pool, and timer utilities.

## Requirements
- Linux, x86/x86-64 — uses raw Linux futex syscalls and x86 intrinsics directly, so it isn't portable to other platforms as-is
- A C++20-capable compiler (developed and tested with GCC 11)
- CMake 3.16+

## Build
```
cmake -S . -B build
cmake --build build
```
AstraLib is header-only. Add `include/` to your project's include path, or consume the `AstraLib` CMake `INTERFACE` target directly (e.g. via `add_subdirectory`/`FetchContent` + `target_link_libraries(your_app PRIVATE AstraLib)`).

## Components
- **Atomic** — `Spinlock`, `PaddedAtomic`, `AtomicFutex`
- **Buffers** — `AtomicRingBuffer`, a lock-free MPMC ring buffer
- **Threading** — `ThreadPool`
- **Pools** — `ThreadSafeIndexPool`
- **Time** — `Timer`, `timeNow`
- **Debug** — `debugMessage`

Include everything at once via `<AstraLib/AstraLib.hpp>`, or pull in individual headers as needed.

## Design Philosophy
Every primitive here made a deliberate tradeoff instead of inheriting a generic default:

- **`Spinlock` / raw atomics vs. `std::mutex`** — for critical sections measured in tens of nanoseconds, a mutex's potential syscall and kernel-level arbitration can cost more than the work it's protecting. `Spinlock` never asks the kernel for anything, trading CPU cycles for latency: its worst case is bounded by how long the lock is actually held, not by scheduler behavior.
- **`AtomicFutex` vs. spinning** — the opposite tradeoff, used where it actually matters: a thread that might genuinely wait a while (a `ThreadPool` worker with no work queued) shouldn't burn a full core doing it. Futex-based waiting parks the thread with the OS instead, trading a small wake-up latency for not wasting resources when there's truly nothing to do.
- **`AtomicRingBuffer`** — a ticket-based, lock-free MPMC design built around a specific communication pattern: producers and consumers sharing a fast interconnect, e.g. cores on the same NUMA node. It isn't trying to be a general-purpose queue — it's tuned for that pattern specifically.

These are design intentions, not published benchmark numbers — take them as the reasoning behind the tradeoffs, not as performance claims.

## Testing
```
ctest --test-dir build --output-on-failure
```
Sanitizer builds:
```
cmake -S . -B build-tsan -DENABLE_TSAN=ON   # data races
cmake -S . -B build-asan -DENABLE_ASAN=ON   # memory errors + UB
```
See [docs/tests.md](docs/tests.md) for full test coverage and known findings.

## Notes 
This project's source code was written entirely by the creator. The AI use is limited to documentation and test code (Test code have been overseen by the creator.). 

## License
Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
