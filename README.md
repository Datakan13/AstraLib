# AstraLib
AstraLib is a C++20 concurrency library focused on performance. It includes atomic helpers, an MPMC ring buffer, a thread-safe debug function, a thread-safe index pool, a thread pool, and timer utilities.

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
