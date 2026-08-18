# Every test gets a hard ctest-level TIMEOUT regardless of any in-binary
# watchdog it has: this repo's primitives spin instead of blocking-with-error
# when something is wrong (see ThreadPool's shutdown hang and AtomicFutex's
# wake-before-wait bug, both found via tests in this suite), so a mistake we
# haven't anticipated should still make ctest SIGKILL the process and report
# a timeout instead of hanging CI forever.
function(astra_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE AstraLib pthread)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

astra_test(SpinlockTest)
astra_test(PaddedAtomicTest)
astra_test(AtomicFutexTest)
astra_test(ThreadSafeIndexPoolTest)
astra_test(ThreadPoolTest)
# Each of its 6 sub-tests has its own internal bounded wait (up to 60s) to
# give the still-open, intermittent task-loss bug room to surface cleanly
# instead of getting cut off mid-diagnosis by a tighter ctest-level kill.
set_tests_properties(ThreadPoolTest PROPERTIES TIMEOUT 240)
astra_test(TimerTest)
astra_test(TimeNowTest)
astra_test(DebugFuncTest)

# Basic functional coverage (isEmpty(), enqueue(const A&), emplaceEnqueue's
# trivially-destructible branch, custom BATCH_SIZE) — distinct from the
# adversarial suite below.
astra_test(AtomicRingBufferTest)

# Adversarial suite: exactly-once MPMC accounting, torn-write detection,
# lifetime canaries, oversubscription, futex lost-wakeup hunting. Has an
# internal watchdog that turns queue deadlocks into diagnosed failures.
astra_test(AtomicRingBufferStressTest)
set_tests_properties(AtomicRingBufferStressTest PROPERTIES TIMEOUT 900)
